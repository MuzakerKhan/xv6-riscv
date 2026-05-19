#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "deadlock.h"

// these are the global variables for the whole deadlock system
// they are defined here and declared as extern in deadlock.h so other files can use them
struct spinlock    dl_lock;                        // protects the resource table from being modified by two cpus at once
struct dl_resource dl_resources[MAX_RESOURCES];    // the main resource table, each slot is one resource
int                dl_nresources = DL_TOKEN_MAX;   // starts at 8 because first 8 slots are reserved for user tokens
int                dl_mode       = DL_MODE_NORMAL; // start in normal mode by default
int                dl_resolution = DL_RES_KILL;    // start with kill as the resolution method
volatile int       dl_check_pending = 0;           // timer sets this to 1 every 100 ticks to trigger a check
int                dl_ready      = 0;              // 0 means system not ready yet, set to 1 in deadlock_init

// one entry per cpu, tells us if that cpu is currently busy inside our deadlock code
// this is needed because if we are inside deadlock code and we acquire another lock
// the hook would try to run deadlock code again on the same cpu which would cause a panic
int dl_cpu_busy[NCPU];

extern struct proc proc[]; // process table defined in proc.c, we need it to check each process state

// grab the deadlock internal lock safely
// we check dl_cpu_busy first so we dont try to acquire the lock twice on the same cpu
// returns 1 if we got the lock, returns 0 if this cpu already holds it
int
dl_lock_acquire(void)
{
  int cpu_id = r_tp(); // r_tp reads the thread pointer register which holds the cpu id
  if(dl_cpu_busy[cpu_id])
    return 0;
  dl_cpu_busy[cpu_id] = 1;
  acquire(&dl_lock);
  return 1;
}

// release the internal lock and mark this cpu as no longer busy
void
dl_lock_release(void)
{
  int cpu_id = r_tp();
  release(&dl_lock);
  dl_cpu_busy[cpu_id] = 0;
}

// called once at boot time from main.c to setup the resource table
// must be called before any sleeplocks or pipes are created so they get registered
void
deadlock_init(void)
{
  int i;
  initlock(&dl_lock, "dl_lock");

  // set all slots to empty (no holder, no name)
  for(i = 0; i < MAX_RESOURCES; i++){
    dl_resources[i].holder_pid  = -1;
    dl_resources[i].res_type    = DL_TYPE_TOKEN;
    dl_resources[i].res_name[0] = '\0';
  }

  // give names to the first 8 slots which are the user tokens (T0 through T7)
  for(i = 0; i < DL_TOKEN_MAX; i++){
    dl_resources[i].res_name[0] = 'T';
    dl_resources[i].res_name[1] = '0' + i;
    dl_resources[i].res_name[2] = '\0';
  }

  dl_ready = 1; // system is ready now, hooks in sleeplock and pipe code will start working
}

// add a new resource to the table and return its assigned id
// called from initsleeplock() and pipealloc() when a new kernel resource is created
// returns -1 if the table is full
int
dl_register(char *name, int type)
{
  int id;
  int i;

  if(!dl_ready)
    return -1; // deadlock system not initalized yet, skip

  if(dl_nresources >= MAX_RESOURCES)
    return -1; // table is full, no more room

  id = dl_nresources++;
  dl_resources[id].holder_pid = -1; // nobody holds it yet
  dl_resources[id].res_type   = type;

  // copy the name character by character since we cant use string.h in kernel
  for(i = 0; i < 15 && name[i]; i++)
    dl_resources[id].res_name[i] = name[i];
  dl_resources[id].res_name[i] = '\0'; // null terminator

  return id;
}

// update the resource table when a process successfully gets a resource
// spinlocks are NOT added to the holds[] array because processes never sleep
// while holding a spinlock so they cannot cause a circular wait deadlock
static void
mark_acquire(int rid, int pid, int type)
{
  int i;
  dl_resources[rid].holder_pid = pid; // record who holds this resource now

  if(type == DL_TYPE_SPINLOCK)
    return; // spinlocks dont count toward deadlock holds, skip

  // find this process in the process table and add the resource to its holds array
  for(i = 0; i < NPROC; i++){
    if(proc[i].pid != pid)
      continue;
    if(proc[i].holds_count < MAX_HOLDS)
      proc[i].holds[proc[i].holds_count++] = rid;
    proc[i].waiting_for  = -1; // got what it was waiting for, clear the wait
    proc[i].dl_preempted = 0;
    break;
  }
}

// update the resource table when a process gives back a resource
// also removes the resource from the process holds[] array
static void
mark_release(int rid, int pid, int type)
{
  int i, k, j;
  dl_resources[rid].holder_pid = -1; // resource is free again

  if(type == DL_TYPE_SPINLOCK)
    return; // spinlocks are not in holds[] so nothing to remove

  for(i = 0; i < NPROC; i++){
    if(proc[i].pid != pid)
      continue;
    // find the rid in holds[] and remove it by shifting everything after it left
    for(k = 0; k < proc[i].holds_count; k++){
      if(proc[i].holds[k] != rid)
        continue;
      for(j = k; j < proc[i].holds_count - 1; j++)
        proc[i].holds[j] = proc[i].holds[j+1];
      proc[i].holds_count--;
      proc[i].holds[proc[i].holds_count] = -1; // clear the last slot
      break;
    }
    break;
  }
}

// called from sleeplock and pipe code when a resource is acquired by a process
// in aggressive mode also runs deadlock detection immediately after
void
dl_on_acquire(int rid, int pid, int type)
{
  if(!dl_ready || rid < 0 || pid <= 0)
    return;
  if(dl_cpu_busy[r_tp()])
    return; // this cpu is already inside deadlock code, skip to avoid re-entry
  if(!dl_lock_acquire())
    return;

  mark_acquire(rid, pid, type);

  // in aggressive mode we check for deadlock after every single acquire
  // this is more overhead but catches deadlocks faster
  if(dl_mode == DL_MODE_AGGRESSIVE && dl_detect_locked())
    dl_resolve_locked(); // this function releases dl_lock internally
  else
    dl_lock_release();
}

// called from sleeplock and pipe code when a process gives back a resource
void
dl_on_release(int rid, int pid, int type)
{
  if(!dl_ready || rid < 0 || pid <= 0)
    return;
  if(dl_cpu_busy[r_tp()])
    return;
  if(!dl_lock_acquire())
    return;

  mark_release(rid, pid, type);
  dl_lock_release();
}

// called just before a process goes to sleep waiting for a resource
// we record waiting_for so the wait-for graph has this edge
void
dl_on_wait(int rid, int pid)
{
  int i;
  if(!dl_ready || rid < 0 || pid <= 0)
    return;
  if(dl_cpu_busy[r_tp()])
    return;
  if(!dl_lock_acquire())
    return;

  for(i = 0; i < NPROC; i++){
    if(proc[i].pid == pid){
      proc[i].waiting_for = rid; // this process is now waiting for resource rid
      break;
    }
  }
  dl_lock_release();
}

// called when a process wakes up and is no longer waiting for a resource
void
dl_on_unwait(int pid)
{
  int i;
  if(!dl_ready || pid <= 0)
    return;
  if(dl_cpu_busy[r_tp()])
    return;
  if(!dl_lock_acquire())
    return;

  for(i = 0; i < NPROC; i++){
    if(proc[i].pid == pid){
      proc[i].waiting_for = -1; // not waiting for anything anymore
      break;
    }
  }
  dl_lock_release();
}

// these two arrays are used during the dfs cycle detection
// vis[] tracks which processes we already visited so we dont visit them twice
// stk[] tracks which processes are in the current dfs path, used to detect a back edge
static int vis[NPROC];
static int stk[NPROC];

// helper to find the index of a process in proc[] by its pid
// returns -1 if not found or process is not active
static int
find_proc_idx(int pid)
{
  int i;
  for(i = 0; i < NPROC; i++){
    if(proc[i].pid == pid && proc[i].state != UNUSED)
      return i;
  }
  return -1;
}

// depth first search from one process
// follows the wait-for graph: process waits for resource, resource held by other process
// if we come back to a node thats already in our current path (stk[]) we found a cycle
// returns 1 if cycle found (deadlock), 0 if no cycle from this node
static int
check_cycle(int idx)
{
  int wait_res;   // which resource this process is waiting for
  int holder_pid; // which process holds that resource
  int next_idx;   // index of the holder in proc[]

  vis[idx] = 1;
  stk[idx] = 1;

  wait_res = proc[idx].waiting_for;
  if(wait_res < 0 || wait_res >= dl_nresources){
    stk[idx] = 0;
    return 0; // this process is not waiting for anything, no edge to follow
  }

  holder_pid = dl_resources[wait_res].holder_pid;
  if(holder_pid <= 0){
    stk[idx] = 0;
    return 0; // resource is free, no one holds it so no edge
  }

  next_idx = find_proc_idx(holder_pid);
  if(next_idx < 0){
    stk[idx] = 0;
    return 0; // holder process not found in table
  }

  // if next_idx is already on our current path then we have a cycle
  if(stk[next_idx])
    return 1; // deadlock confirmed

  if(!vis[next_idx] && check_cycle(next_idx))
    return 1;

  stk[idx] = 0;
  return 0;
}

// run cycle detection on the entire wait-for graph
// goes through every active process and runs dfs from each unvisited one
// caller must hold dl_lock before calling this
// returns 1 if deadlock found, 0 if system is fine
int
dl_detect_locked(void)
{
  int i;

  // reset both arrays before each full detection run
  for(i = 0; i < NPROC; i++){
    vis[i] = 0;
    stk[i] = 0;
  }

  for(i = 0; i < NPROC; i++){
    if(proc[i].state != UNUSED && proc[i].pid > 0 && !vis[i]){
      if(check_cycle(i))
        return 1; // found a cycle, stop immediately and report deadlock
    }
  }
  return 0; // no cycle found, all good
}

// banker's algorithm: check if it is safe to give resource rid to process pid
// we simulate giving the resource then check if a deadlock would form
// if it would cause deadlock we return 0 (unsafe), otherwise return 1 (safe)
// caller must hold dl_lock
int
dl_banker_safe_locked(int pid, int rid)
{
  int i;
  int old_holder; // save original holder so we can undo the simulation
  int added;      // did we add to holds array during simulation
  int old_wait;   // save original waiting_for so we can undo
  int result;
  struct proc *p = 0;

  // temporarily pretend this process got the resource
  old_holder = dl_resources[rid].holder_pid;
  dl_resources[rid].holder_pid = pid;

  // find the process struct for this pid
  for(i = 0; i < NPROC; i++){
    if(proc[i].pid == pid){
      p = &proc[i];
      break;
    }
  }

  if(!p){
    dl_resources[rid].holder_pid = old_holder;
    return 0; // process not found, unsafe by default
  }

  // temporarily add this resource to its holds array
  added = 0;
  if(p->holds_count < MAX_HOLDS){
    p->holds[p->holds_count++] = rid;
    added = 1;
  }

  // pretend it is no longer waiting (it got the resource)
  old_wait = p->waiting_for;
  p->waiting_for = -1;

  // if no cycle after the simulated grant then state is safe
  result = !dl_detect_locked();

  // undo all the simulation changes
  dl_resources[rid].holder_pid = old_holder;
  p->waiting_for = old_wait;
  if(added){
    p->holds_count--;
    p->holds[p->holds_count] = -1;
  }

  return result;
}

// calculate the kill score for a process
// higher score = we want to kill this process first
// formula uses three factors weighted differently
static int
get_score(struct proc *p)
{
  int prio_part; // contribution from priority
  int prog_part; // contribution from progress (cpu time used)
  int hold_part; // contribution from number of resources held
  int prog_inv;  // inverse of progress (less progress = higher score)
  uint64 ticks;

  // low priority processes score higher here because (9 - low_prio) is bigger
  prio_part = (9 - p->priority) * W_PRIO;

  // processes that used less cpu time score higher here (less work done = cheaper to kill)
  ticks = p->cpu_ticks;
  if(ticks > PROG_CAP)
    ticks = PROG_CAP; // cap it so very old processes dont dominate
  prog_inv  = (int)((PROG_CAP - ticks) * 100 / PROG_CAP); // 100 for new process, 0 for old
  prog_part = prog_inv * W_PROG;

  // more resources held means more connected to the deadlock
  hold_part = p->holds_count * W_HOLD;

  return prio_part + prog_part + hold_part;
}

// pick the best victim process and break the deadlock
// caller must hold dl_lock, this function releases it before returning
// it releases BEFORE calling kkill because kkill internally acquires proc locks
// and if we still held dl_lock that would cause a lock ordering problem
void
dl_resolve_locked(void)
{
  int i, k;
  int victim_pid; // pid of the process we decide to kill or preempt
  int max_score;  // highest score seen so far
  int cur_score;  // score of the current process we are checking
  int res_id;     // resource id when freeing held resources
  int action;     // save the resolution mode before releasing the lock

  victim_pid = -1;
  max_score  = -1;

  // go through all waiting processes and find the one with the highest kill score
  for(i = 0; i < NPROC; i++){
    if(proc[i].state == UNUSED || proc[i].pid <= 0)
      continue;
    if(proc[i].waiting_for < 0)
      continue; // not blocked on our system, skip
    cur_score = get_score(&proc[i]);
    if(cur_score > max_score){
      max_score  = cur_score;
      victim_pid = proc[i].pid;
    }
  }

  if(victim_pid <= 0){
    dl_lock_release();
    return; // no victim found, nothing to do
  }

  // find the victim and free all resources it was holding
  // also wake up any processes that were waiting for those resources
  for(i = 0; i < NPROC; i++){
    if(proc[i].pid != victim_pid)
      continue;
    for(k = 0; k < proc[i].holds_count; k++){
      res_id = proc[i].holds[k];
      if(res_id >= 0 && res_id < dl_nresources){
        dl_resources[res_id].holder_pid = -1;
        wakeup(&dl_resources[res_id]); // wake up anyone waiting for this resource
      }
    }
    proc[i].holds_count  = 0;
    proc[i].waiting_for  = -1;
    if(dl_resolution == DL_RES_PREEMPT)
      proc[i].dl_preempted = 1; // signal to the process that it was preempted
    break;
  }

  action = dl_resolution; // save before releasing lock

  dl_lock_release(); // release the lock NOW before calling kkill

  if(action == DL_RES_KILL)
    kkill(victim_pid); // kill the victim process

  printf("DEADLOCK RESOLVED: action=%s victim=pid%d score=%d\n",
    action == DL_RES_KILL ? "KILL" : "PREEMPT",
    victim_pid, max_score);
}

// called from freeproc() in proc.c when any process exits
// makes sure any resources it held are freed in our table
// prevents resources from being stuck as held forever if a process crashes
void
dl_proc_cleanup(int pid)
{
  int i;
  if(!dl_ready || pid <= 0)
    return;
  if(!dl_lock_acquire())
    return;

  for(i = 0; i < dl_nresources; i++){
    if(dl_resources[i].holder_pid == pid){
      dl_resources[i].holder_pid = -1;
      wakeup(&dl_resources[i]); // wake up waiters
    }
  }
  dl_lock_release();
}

// called from sys_dlacquire and sys_dlstate to run a pending periodic check
// the timer sets dl_check_pending to 1 every 100 ticks (about 1 second)
// we run detection here to catch any deadlocks that werent caught by on-acquire hooks
void
dl_maybe_check(void)
{
  if(!dl_ready || !dl_check_pending)
    return;
  dl_check_pending = 0;
  if(!dl_lock_acquire())
    return;
  if(dl_detect_locked())
    dl_resolve_locked(); // resolve releases the lock internally
  else
    dl_lock_release();
}

// print everything we know about the current deadlock system state
// also runs banker's check for waiting processes and runs detection at the end
void
dl_print_state(void)
{
  int i;
  int found_held; // did we find any held resources to print
  int found_wait; // is any process currently waiting
  int res_id;
  int safe;
  char *type_name;

  dl_maybe_check(); // run any pending periodic check first

  if(!dl_lock_acquire())
    return;

  printf("\n[DEADLOCK SUBSYSTEM]\n");
  printf("Mode: %s\n",
    dl_mode == DL_MODE_AGGRESSIVE ? "AGGRESSIVE" : "NORMAL");
  printf("Resolution: %s\n",
    dl_resolution == DL_RES_KILL ? "KILL" : "PREEMPT");

  printf("\nResources currently held:\n");
  found_held = 0;
  for(i = 0; i < dl_nresources; i++){
    if(dl_resources[i].holder_pid < 0)
      continue; // resource is free, skip
    found_held = 1;
    type_name = "token";
    if(dl_resources[i].res_type == DL_TYPE_SLEEPLOCK)
      type_name = "sleeplock";
    if(dl_resources[i].res_type == DL_TYPE_PIPE)
      type_name = "pipe";
    printf("  resource %d (%s %s) held by pid %d\n",
      i, type_name, dl_resources[i].res_name,
      dl_resources[i].holder_pid);
  }
  if(!found_held)
    printf("  (none)\n");

  printf("\nProcess table:\n");
  for(i = 0; i < NPROC; i++){
    if(proc[i].state == UNUSED || proc[i].pid <= 0)
      continue;
    printf("  pid %d  name=%s  priority=%d  cpu_ticks=%lu  holds=%d  waiting_for=%d  kill_score=%d\n",
      proc[i].pid, proc[i].name,
      proc[i].priority,
      (unsigned long)proc[i].cpu_ticks,
      proc[i].holds_count,
      proc[i].waiting_for,
      get_score(&proc[i]));
  }

  // check if any process is currently blocked waiting for a resource
  found_wait = 0;
  for(i = 0; i < NPROC; i++){
    if(proc[i].state != UNUSED && proc[i].pid > 0 && proc[i].waiting_for >= 0){
      found_wait = 1;
      break;
    }
  }

  // if there are waiting processes run banker's check on each one
  if(found_wait){
    printf("\nBanker's algorithm check:\n");
    for(i = 0; i < NPROC; i++){
      if(proc[i].state == UNUSED || proc[i].pid <= 0)
        continue;
      res_id = proc[i].waiting_for;
      if(res_id < 0)
        continue;
      safe = dl_banker_safe_locked(proc[i].pid, res_id);
      printf("  pid %d requesting resource %d -> state would be %s\n",
        proc[i].pid, res_id, safe ? "SAFE" : "UNSAFE");
    }
  }

  // run final detection and resolve if needed
  if(dl_detect_locked()){
    printf("\nDEADLOCK DETECTED\n");
    dl_resolve_locked(); // releases the lock
  } else {
    printf("\nResult: no deadlock detected\n");
    printf("[end of report]\n\n");
    dl_lock_release();
  }
}
