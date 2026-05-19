#include "types.h"
#include "riscv.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "deadlock.h"

extern int dl_cpu_busy[];

// system call: print the current deadlock system state
uint64
sys_dlstate(void)
{
  dl_print_state();
  return 0;
}

// system call: acquire a user token (resource 0 to 7)
// blocks until the token is available
// return values:
//   0  = got it
//  -1  = bad token number
//  -2  = this process was killed as the deadlock victim
//  -3  = resources were preempted (stripped), process can retry
uint64
sys_dlacquire(void)
{
  int token_id;
  struct proc *p;

  argint(0, &token_id);
  if(token_id < 0 || token_id >= DL_TOKEN_MAX)
    return -1;

  p = myproc();

  if(!dl_lock_acquire())
    return -1;

  // keep trying until we get the token
  while(dl_resources[token_id].holder_pid != -1){

    // mark that we are waiting for this token
    p->waiting_for = token_id;

    // check if we are now part of a deadlock cycle
    if(dl_detect_locked()){
      printf("DEADLOCK DETECTED: pid=%d waiting for token %d\n",
        p->pid, token_id);
      dl_resolve_locked();
      p->waiting_for = -1;

      if(p->dl_preempted){
        p->dl_preempted = 0;
        return -3;
      }
      if(killed(p))
        return -2;

      // try to get the lock again and retry the loop
      if(!dl_lock_acquire())
        return -1;
      continue;
    }

    // run banker's check and warn if granting this would be unsafe
    if(!dl_banker_safe_locked(p->pid, token_id))
      printf("BANKER WARNING: giving token %d to pid=%d is UNSAFE\n",
        token_id, p->pid);

    // go to sleep until someone releases this token
    // we clear the cpu busy flag first so other processes on this cpu
    // can still use the deadlock system while we are sleeping
    dl_cpu_busy[r_tp()] = 0;
    sleep(&dl_resources[token_id], &dl_lock);
    dl_cpu_busy[r_tp()] = 1;

    // check what happened when we woke up
    if(p->dl_preempted){
      p->waiting_for   = -1;
      p->dl_preempted  = 0;
      dl_lock_release();
      return -3;
    }
    if(killed(p)){
      p->waiting_for = -1;
      dl_lock_release();
      return -2;
    }
  }

  // token is free now, take it
  dl_resources[token_id].holder_pid = p->pid;
  if(p->holds_count < MAX_HOLDS)
    p->holds[p->holds_count++] = token_id;
  p->waiting_for = -1;

  dl_lock_release();
  return 0;
}

// system call: release a token that this process holds
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

  // make sure we actually hold this token
  if(dl_resources[token_id].holder_pid != p->pid){
    dl_lock_release();
    return -1;
  }

  // remove token from our holds array by shifting everything left
  for(i = 0; i < p->holds_count; i++){
    if(p->holds[i] != token_id)
      continue;
    for(j = i; j < p->holds_count - 1; j++)
      p->holds[j] = p->holds[j+1];
    p->holds_count--;
    p->holds[p->holds_count] = -1;
    break;
  }

  dl_resources[token_id].holder_pid = -1;

  // wake up anyone who was waiting for this token
  wakeup(&dl_resources[token_id]);

  dl_lock_release();
  return 0;
}

// system call: switch between normal and aggressive mode
// 0 = normal, 1 = aggressive
uint64
sys_dlsetmode(void)
{
  int new_mode;
  argint(0, &new_mode);

  if(new_mode != DL_MODE_NORMAL && new_mode != DL_MODE_AGGRESSIVE)
    return -1;

  dl_mode = new_mode;
  printf("Deadlock mode set to: %s\n",
    new_mode == DL_MODE_AGGRESSIVE ? "AGGRESSIVE" : "NORMAL");
  return 0;
}

// system call: switch between kill and preempt resolution
// 0 = kill the victim, 1 = strip resources and keep it alive
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

// system call: set the priority of the calling process
// 0 is lowest priority (killed first), 9 is highest (protected)
uint64
sys_setpriority(void)
{
  int prio;
  argint(0, &prio);

  if(prio < 0 || prio > 9)
    return -1;

  myproc()->priority = prio;
  return 0;
}
