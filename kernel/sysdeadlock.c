#include "types.h"
#include "riscv.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "deadlock.h"

extern int dl_cpu_busy[];

// ── dlstate() ────────────────────────────────────────────────────────────────
uint64
sys_dlstate(void)
{
  dl_print_state();
  return 0;
}

// ── dlacquire(int rid) ───────────────────────────────────────────────────────
// Acquire one of the DL_TOKEN_MAX explicit user tokens (rid 0..7).
// Returns:
//   0  = success
//  -1  = bad rid
//  -2  = killed as deadlock victim
//  -3  = preempted (resources stripped; retry later)
uint64
sys_dlacquire(void)
{
  int rid;
  argint(0, &rid);
  if(rid < 0 || rid >= DL_TOKEN_MAX) return -1;

  struct proc *p = myproc();

  // Use the exported wrapper so dl_cpu_busy is set correctly.
  if(!dl_lock_acquire()) return -1;

  while(dl_resources[rid].holder_pid != -1){
    p->waiting_for = rid;

    if(dl_detect_locked()){
      printf("DEADLOCK DETECTED: pid=%d waiting for token %d\n", p->pid, rid);
      dl_resolve_locked();   // releases dl_lock internally
      p->waiting_for = -1;
      if(p->dl_preempted){ p->dl_preempted = 0; return -3; }
      if(killed(p)) return -2;
      if(!dl_lock_acquire()) return -1;
      continue;  // dl_cpu_busy[cpu]=1 set by dl_lock_acquire
    }

    if(!dl_banker_safe_locked(p->pid, rid))
      printf("BANKER WARNING: token %d to pid=%d is UNSAFE\n", rid, p->pid);

    // sleep() releases dl_lock and suspends this process.
    // Clear dl_cpu_busy BEFORE sleeping — other processes will run on this
    // CPU and must not inherit our "busy" state.
    // After sleep() returns, dl_lock is reacquired; restore the flag.
    dl_cpu_busy[r_tp()] = 0;
    sleep(&dl_resources[rid], &dl_lock);
    dl_cpu_busy[r_tp()] = 1;

    if(p->dl_preempted){ p->waiting_for=-1; p->dl_preempted=0;
                         dl_lock_release(); return -3; }
    if(killed(p)){ p->waiting_for=-1; dl_lock_release(); return -2; }
  }

  dl_resources[rid].holder_pid = p->pid;
  if(p->holds_count < MAX_HOLDS)
    p->holds[p->holds_count++] = rid;
  p->waiting_for = -1;

  dl_lock_release();
  return 0;
}

// ── dlrelease(int rid) ───────────────────────────────────────────────────────
uint64
sys_dlrelease(void)
{
  int rid;
  argint(0, &rid);
  if(rid < 0 || rid >= DL_TOKEN_MAX) return -1;

  struct proc *p = myproc();
  if(!dl_lock_acquire()) return -1;

  if(dl_resources[rid].holder_pid != p->pid){
    dl_lock_release();
    return -1;
  }

  for(int i = 0; i < p->holds_count; i++){
    if(p->holds[i] != rid) continue;
    for(int j = i; j < p->holds_count-1; j++)
      p->holds[j] = p->holds[j+1];
    p->holds_count--;
    p->holds[p->holds_count] = -1;
    break;
  }
  dl_resources[rid].holder_pid = -1;
  wakeup(&dl_resources[rid]);

  dl_lock_release();
  return 0;
}

// ── dlsetmode(int mode) ──────────────────────────────────────────────────────
uint64
sys_dlsetmode(void)
{
  int mode;
  argint(0, &mode);
  if(mode != DL_MODE_NORMAL && mode != DL_MODE_AGGRESSIVE) return -1;
  dl_mode = mode;
  printf("Deadlock mode: %s\n",
         mode == DL_MODE_AGGRESSIVE ? "AGGRESSIVE" : "NORMAL");
  return 0;
}

// ── dlsetresolution(int res) ─────────────────────────────────────────────────
uint64
sys_dlsetresolution(void)
{
  int res;
  argint(0, &res);
  if(res != DL_RES_KILL && res != DL_RES_PREEMPT) return -1;
  dl_resolution = res;
  printf("Deadlock resolution: %s\n",
         res == DL_RES_KILL ? "KILL" : "PREEMPT");
  return 0;
}

// ── setpriority(int prio) ────────────────────────────────────────────────────
uint64
sys_setpriority(void)
{
  int prio;
  argint(0, &prio);
  if(prio < 0 || prio > 9) return -1;
  myproc()->priority = prio;
  return 0;
}
