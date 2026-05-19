#include "types.h"
#include "riscv.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "deadlock.h"

// dl_cpu_busy is defined in deadlock.c but we need it here too
// extern just means "this variable exists somewhere else, use it"
extern int dl_cpu_busy[];

// system call: dlstate()
// user calls this to print the current state of the deadlock system
// shows all resources, all processes and their scores, and runs detection
uint64
sys_dlstate(void)
{
  dl_print_state();
  return 0;
}

// system call: dlacquire(token_id)
// user program calls this to grab a token (resource 0 to 7)
// if the token is held by another process, this blocks until it becomes free
// return values:
//   0  = got the token successfully
//  -1  = bad token number (must be 0 to 7)
//  -2  = this process was chosen as the deadlock victim and killed
//  -3  = preempt mode was on, our resources were stripped (process can still run)
uint64
sys_dlacquire(void)
{
  int token_id;
  struct proc *p;

  argint(0, &token_id); // get the argument the user passed (token number)
  if(token_id < 0 || token_id >= DL_TOKEN_MAX)
    return -1; // invalid token number

  p = myproc(); // get the current process struct

  if(!dl_lock_acquire())
    return -1;

  // keep looping until we get the token or something goes wrong
  while(dl_resources[token_id].holder_pid != -1){

    // record that we are waiting for this token
    // this is what creates the edge in the wait-for graph
    p->waiting_for = token_id;

    // run deadlock detection to see if we just created a cycle
    if(dl_detect_locked()){
      printf("DEADLOCK DETECTED: pid=%d waiting for token %d\n", p->pid, token_id);
      dl_resolve_locked(); // this releases dl_lock internally
      p->waiting_for = -1;

      if(p->dl_preempted){
        p->dl_preempted = 0;
        return -3; // we were the preempt victim
      }
      if(killed(p))
        return -2; // we were killed as the victim

      // lock was released by resolve, grab it again and retry
      if(!dl_lock_acquire())
        return -1;
      continue;
    }

    // run banker's check to see if granting this would lead to an unsafe state
    // we just print a warning, we dont block the request here
    if(!dl_banker_safe_locked(p->pid, token_id))
      printf("BANKER WARNING: giving token %d to pid=%d is UNSAFE\n", token_id, p->pid);

    // go to sleep until someone releases the token
    // important: we clear dl_cpu_busy before sleeping so other processes on this cpu
    // can still use the deadlock system while we are asleep
    // if we dont clear it, the flag would be stuck at 1 for this cpu even while sleeping
    dl_cpu_busy[r_tp()] = 0;
    sleep(&dl_resources[token_id], &dl_lock); // sleep releases dl_lock and reaquires it when woken
    dl_cpu_busy[r_tp()] = 1; // restore flag after waking up since we hold dl_lock again

    // check what happened while we were sleeping
    if(p->dl_preempted){
      p->waiting_for   = -1;
      p->dl_preempted  = 0;
      dl_lock_release();
      return -3; // resources were stripped in preempt mode
    }
    if(killed(p)){
      p->waiting_for = -1;
      dl_lock_release();
      return -2; // we were killed by the resolver
    }
  }

  // token is free, take it
  dl_resources[token_id].holder_pid = p->pid;
  if(p->holds_count < MAX_HOLDS)
    p->holds[p->holds_count++] = token_id;
  p->waiting_for = -1;

  dl_lock_release();
  return 0; // success
}

// system call: dlrelease(token_id)
// user program calls this to give back a token it holds
// also wakes up any process that was waiting for this token
uint64
sys_dlrelease(void)
{
  int token_id;
  int i, j;
  struct proc *p;

  argint(0, &token_id);
  if(token_id < 0 || token_id >= DL_TOKEN_MAX)
    return -1;

  p = myproc();

  if(!dl_lock_acquire())
    return -1;

  // make sure this process actually holds the token before releasing
  if(dl_resources[token_id].holder_pid != p->pid){
    dl_lock_release();
    return -1; // we dont hold it, cant release
  }

  // remove the token from the process holds[] array
  // we do this by finding it and shifting everything after it one step left
  for(i = 0; i < p->holds_count; i++){
    if(p->holds[i] != token_id)
      continue;
    for(j = i; j < p->holds_count - 1; j++)
      p->holds[j] = p->holds[j+1];
    p->holds_count--;
    p->holds[p->holds_count] = -1;
    break;
  }

  dl_resources[token_id].holder_pid = -1; // mark as free

  wakeup(&dl_resources[token_id]); // wake up anyone sleeping waiting for this token

  dl_lock_release();
  return 0;
}

// system call: dlsetmode(mode)
// switches between normal mode (0) and aggressive mode (1)
// this setting persists until the user changes it again
// only the user can change it, nothing automatic changes it
uint64
sys_dlsetmode(void)
{
  int new_mode;
  argint(0, &new_mode);

  if(new_mode != DL_MODE_NORMAL && new_mode != DL_MODE_AGGRESSIVE)
    return -1; // invalid mode number

  dl_mode = new_mode;
  printf("Deadlock mode set to: %s\n",
    new_mode == DL_MODE_AGGRESSIVE ? "AGGRESSIVE" : "NORMAL");
  return 0;
}

// system call: dlsetresolution(res)
// switches between kill mode (0) and preempt mode (1)
// kill: victim process is terminated
// preempt: victim loses resources but keeps running, gets error code -3
// this setting also persists until user changes it
uint64
sys_dlsetresolution(void)
{
  int new_res;
  argint(0, &new_res);

  if(new_res != DL_RES_KILL && new_res != DL_RES_PREEMPT)
    return -1;

  dl_resolution = new_res;
  printf("Deadlock resolution set to: %s\n",
    new_res == DL_RES_KILL ? "KILL" : "PREEMPT");
  return 0;
}

// system call: setpriority(prio)
// sets the kill priority of the calling process
// 0 means lowest priority (will be killed first in a deadlock)
// 9 means highest priority (protected, killed last)
// used to demonstrate the scoring system in dltest3
uint64
sys_setpriority(void)
{
  int prio;
  argint(0, &prio);

  if(prio < 0 || prio > 9)
    return -1; // must be 0 to 9

  myproc()->priority = prio;
  return 0;
}
