#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "deadlock.h"

// global resource table and state variables
struct spinlock    dl_lock;
struct dl_resource dl_resources[MAX_RESOURCES];
int                dl_nresources = DL_TOKEN_MAX;
int                dl_mode       = DL_MODE_NORMAL;
int                dl_resolution = DL_RES_KILL;
volatile int       dl_check_pending = 0;
int                dl_ready      = 0;

// one flag per cpu, set to 1 when that cpu is inside our subsystem
// this stops re-entry problems when printf or wakeup tries to call back in
int dl_cpu_busy[NCPU];

extern struct proc proc[];

// grab the deadlock lock safely
// returns 0 if this cpu already holds it (to avoid double acquire panic)
int
dl_lock_acquire(void)
{
  int cpu_id = r_tp();
  if(dl_cpu_busy[cpu_id])
    return 0;
  dl_cpu_busy[cpu_id] = 1;
  acquire(&dl_lock);
  return 1;
}

// release the deadlock lock and clear the busy flag
void
dl_lock_release(void)
{
  int cpu_id = r_tp();
  release(&dl_lock);
  dl_cpu_busy[cpu_id] = 0;
}

// called once at kernel startup to setup the resource table
void
deadlock_init(void)
{
  int i;
  initlock(&dl_lock, "dl_lock");

  // mark all slots as empty
  for(i = 0; i < MAX_RESOURCES; i++){
    dl_resources[i].holder_pid = -1;
    dl_resources[i].res_type   = DL_TYPE_TOKEN;
    dl_resources[i].res_name[0] = '\0';
  }

  // label the first 8 slots as token0 through token7
  for(i = 0; i < DL_TOKEN_MAX; i++){
    dl_resources[i].res_name[0] = 'T';
    dl_resources[i].res_name[1] = '0' + i;
    dl_resources[i].res_name[2] = '\0';
  }

  dl_ready = 1;
}

// add a new resource to the table and return its id
// called when a sleeplock or pipe is created
int
dl_register(char *name, int type)
{
  int id;
  int i;

  if(!dl_ready)
    return -1;
  if(dl_nresources >= MAX_RESOURCES)
    return -1;

  id = dl_nresources++;
  dl_resources[id].holder_pid = -1;
  dl_resources[id].res_type   = type;

  // copy the name manually since we cant use string library in kernel
  for(i = 0; i < 15 && name[i]; i++)
    dl_resources[id].res_name[i] = name[i];
  dl_resources[id].res_name[i] = '\0';

  return id;
}

// update the table when a process gets a resource
// spinlocks are not added to the holds array because
// processes never sleep while holding a spinlock so they cant deadlock
static void
mark_acquire(int rid, int pid, int type)
{
  int i;
  dl_resources[rid].holder_pid = pid;

  if(type == DL_TYPE_SPINLOCK)
    return;

  for(i = 0; i < NPROC; i++){
    if(proc[i].pid != pid)
      continue;
    if(proc[i].holds_count < MAX_HOLDS)
      proc[i].holds[proc[i].holds_count++] = rid;
    proc[i].waiting_for  = -1;
    proc[i].dl_preempted = 0;
    break;
  }
}

// update the table when a process releases a resource
static void
mark_release(int rid, int pid, int type)
{
  int i, k, j;
  dl_resources[rid].holder_pid = -1;

  if(type == DL_TYPE_SPINLOCK)
    return;

  for(i = 0; i < NPROC; i++){
    if(proc[i].pid != pid)
      continue;
    // remove this resource id from the holds array
    for(k = 0; k < proc[i].holds_count; k++){
      if(proc[i].holds[k] != rid)
        continue;
      for(j = k; j < proc[i].holds_count - 1; j++)
        proc[i].holds[j] = proc[i].holds[j+1];
      proc[i].holds_count--;
      proc[i].holds[proc[i].holds_count] = -1;
      break;
    }
    break;
  }
}

// called from sleeplock and pipe code when a resource is acquired
void
dl_on_acquire(int rid, int pid, int type)
{
  if(!dl_ready || rid < 0 || pid <= 0)
    return;
  if(dl_cpu_busy[r_tp()])
    return;
  if(!dl_lock_acquire())
    return;

  mark_acquire(rid, pid, type);

  // in aggressive mode run detection right after every acquire
  if(dl_mode == DL_MODE_AGGRESSIVE && dl_detect_locked())
    dl_resolve_locked();
  else
    dl_lock_release();
}

// called from sleeplock and pipe code when a resource is released
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
      proc[i].waiting_for = rid;
      break;
    }
  }
  dl_lock_release();
}

// called when a process wakes up and is no longer waiting
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
      proc[i].waiting_for = -1;
      break;
    }
  }
  dl_lock_release();
}

// arrays used during cycle detection
// vis keeps track of which nodes we visited
// stk keeps track of nodes in the current path (for cycle detection)
static int vis[NPROC];
static int stk[NPROC];

// find the index of a process in proc[] by its pid
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

// depth first search from process at index idx
// returns 1 if we find a cycle (deadlock), 0 if no cycle
static int
check_cycle(int idx)
{
  int wait_res;
  int holder_pid;
  int next_idx;

  vis[idx] = 1;
  stk[idx] = 1;

  // which resource is this process waiting for
  wait_res = proc[idx].waiting_for;
  if(wait_res < 0 || wait_res >= dl_nresources){
    stk[idx] = 0;
    return 0;
  }

  // who holds that resource
  holder_pid = dl_resources[wait_res].holder_pid;
  if(holder_pid <= 0){
    stk[idx] = 0;
    return 0;
  }

  next_idx = find_proc_idx(holder_pid);
  if(next_idx < 0){
    stk[idx] = 0;
    return 0;
  }

  // if next node is already in our current path then we found a cycle
  if(stk[next_idx])
    return 1;

  if(!vis[next_idx] && check_cycle(next_idx))
    return 1;

  stk[idx] = 0;
  return 0;
}

// run cycle detection on the whole wait-for graph
// caller must hold dl_lock
// returns 1 if deadlock found, 0 if everything is fine
int
dl_detect_locked(void)
{
  int i;

  // reset visited and stack arrays
  for(i = 0; i < NPROC; i++){
    vis[i] = 0;
    stk[i] = 0;
  }

  for(i = 0; i < NPROC; i++){
    if(proc[i].state != UNUSED && proc[i].pid > 0 && !vis[i]){
      if(check_cycle(i))
        return 1;
    }
  }
  return 0;
}

// banker's algorithm check
// simulates giving resource rid to process pid
// then checks if deadlock would occur
// returns 1 if safe to give, 0 if it would cause deadlock
// caller must hold dl_lock
int
dl_banker_safe_locked(int pid, int rid)
{
  int i;
  int old_holder;
  int added;
  int old_wait;
  int result;
  struct proc *p = 0;

  // temporarily pretend this process got the resource
  old_holder = dl_resources[rid].holder_pid;
  dl_resources[rid].holder_pid = pid;

  for(i = 0; i < NPROC; i++){
    if(proc[i].pid == pid){
      p = &proc[i];
      break;
    }
  }

  if(!p){
    dl_resources[rid].holder_pid = old_holder;
    return 0;
  }

  added = 0;
  if(p->holds_count < MAX_HOLDS){
    p->holds[p->holds_count++] = rid;
    added = 1;
  }

  old_wait = p->waiting_for;
  p->waiting_for = -1;

  // if no cycle then state is safe
  result = !dl_detect_locked();

  // undo the simulation
  dl_resources[rid].holder_pid = old_holder;
  p->waiting_for = old_wait;
  if(added){
    p->holds_count--;
    p->holds[p->holds_count] = -1;
  }

  return result;
}

// calculate how much we want to kill this process
// higher score means we prefer to kill it
// based on: low priority + less progress + more resources held
static int
get_score(struct proc *p)
{
  int prio_part;
  int prog_part;
  int hold_part;
  int prog_inv;
  uint64 ticks;

  // low priority processes have high score
  prio_part = (9 - p->priority) * W_PRIO;

  // processes that ran less get higher score (less progress lost if killed)
  ticks = p->cpu_ticks;
  if(ticks > PROG_CAP)
    ticks = PROG_CAP;
  prog_inv  = (int)((PROG_CAP - ticks) * 100 / PROG_CAP);
  prog_part = prog_inv * W_PROG;

  // processes holding more resources contribute more to the deadlock
  hold_part = p->holds_count * W_HOLD;

  return prio_part + prog_part + hold_part;
}

// pick the best victim and resolve the deadlock
// must be called with dl_lock held
// releases dl_lock before calling kkill to avoid lock ordering problems
void
dl_resolve_locked(void)
{
  int i, k;
  int victim_pid;
  int max_score;
  int cur_score;
  int res_id;
  int action;

  victim_pid = -1;
  max_score  = -1;

  // find the process with the highest kill score
  for(i = 0; i < NPROC; i++){
    if(proc[i].state == UNUSED || proc[i].pid <= 0)
      continue;
    if(proc[i].waiting_for < 0)
      continue;
    cur_score = get_score(&proc[i]);
    if(cur_score > max_score){
      max_score  = cur_score;
      victim_pid = proc[i].pid;
    }
  }

  if(victim_pid <= 0){
    dl_lock_release();
    return;
  }

  // free all resources that the victim was holding
  // and wake up anyone waiting for those resources
  for(i = 0; i < NPROC; i++){
    if(proc[i].pid != victim_pid)
      continue;
    for(k = 0; k < proc[i].holds_count; k++){
      res_id = proc[i].holds[k];
      if(res_id >= 0 && res_id < dl_nresources){
        dl_resources[res_id].holder_pid = -1;
        wakeup(&dl_resources[res_id]);
      }
    }
    proc[i].holds_count  = 0;
    proc[i].waiting_for  = -1;
    if(dl_resolution == DL_RES_PREEMPT)
      proc[i].dl_preempted = 1;
    break;
  }

  action = dl_resolution;

  // must release the lock before calling kkill
  // because kkill internally acquires proc locks
  dl_lock_release();

  if(action == DL_RES_KILL)
    kkill(victim_pid);

  printf("DEADLOCK RESOLVED: action=%s victim=pid%d score=%d\n",
    action == DL_RES_KILL ? "KILL" : "PREEMPT",
    victim_pid, max_score);
}

// called from freeproc when a process exits
// releases any resources it still held in our table
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
      wakeup(&dl_resources[i]);
    }
  }
  dl_lock_release();
}

// runs a deadlock check if the timer set the pending flag
void
dl_maybe_check(void)
{
  if(!dl_ready || !dl_check_pending)
    return;
  dl_check_pending = 0;
  if(!dl_lock_acquire())
    return;
  if(dl_detect_locked())
    dl_resolve_locked();
  else
    dl_lock_release();
}

// print the full system state to the console
// also runs detection and resolves if a deadlock is found
void
dl_print_state(void)
{
  int i;
  int found_held;
  int found_wait;
  int res_id;
  int safe;
  char *type_name;

  dl_maybe_check();

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
      continue;
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
    printf("  pid %d  name=%s  priority=%d  cpu_ticks=%lu"
           "  holds=%d  waiting_for=%d  kill_score=%d\n",
      proc[i].pid, proc[i].name,
      proc[i].priority,
      (unsigned long)proc[i].cpu_ticks,
      proc[i].holds_count,
      proc[i].waiting_for,
      get_score(&proc[i]));
  }

  // check if any process is currently waiting
  found_wait = 0;
  for(i = 0; i < NPROC; i++){
    if(proc[i].state != UNUSED && proc[i].pid > 0 && proc[i].waiting_for >= 0){
      found_wait = 1;
      break;
    }
  }

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

  if(dl_detect_locked()){
    printf("\nDEADLOCK DETECTED\n");
    dl_resolve_locked();
  } else {
    printf("\nResult: no deadlock detected\n");
    printf("[end of report]\n\n");
    dl_lock_release();
  }
}
