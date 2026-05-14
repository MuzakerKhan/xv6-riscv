#include "types.h"
#include "riscv.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "deadlock.h"

// ── dlstate() ────────────────────────────────────────────────────────────────
uint64
sys_dlstate(void)
{
  dl_print_state();
  return 0;
}

// ── dlacquire(int rid) ───────────────────────────────────────────────────────
// Acquire one of the DL_TOKEN_MAX explicit user tokens (rid 0..7).
// Blocks until the resource is free. Returns:
//   0  = success
//  -1  = bad rid
//  -2  = this process was killed as the deadlock victim
//  -3  = this process was preempted (resources stripped; retry later)
uint64
sys_dlacquire(void)
{
  int rid;
  argint(0, &rid);
  if(rid < 0 || rid >= DL_TOKEN_MAX) return -1;

  struct proc *p = myproc();
  acquire(&dl_lock);

  while(dl_resources[rid].holder_pid != -1){
    p->waiting_for = rid;

    // Check for deadlock before sleeping.
    if(dl_detect_locked()){
      printf("DEADLOCK DETECTED: pid=%d waiting for token %d\n", p->pid, rid);
      dl_resolve_locked();   // releases dl_lock
      p->waiting_for = -1;
      if(p->dl_preempted){ p->dl_preempted=0; return -3; }
      if(killed(p)) return -2;
      // Resolution freed the resource; re-acquire dl_lock and retry.
      acquire(&dl_lock);
      continue;
    }

    // Banker's warning (non-blocking).
    if(!dl_banker_safe_locked(p->pid, rid))
      printf("BANKER WARNING: granting token %d to pid=%d is UNSAFE\n",
             rid, p->pid);

    sleep(&dl_resources[rid], &dl_lock);

    // Woke up — check why.
    if(p->dl_preempted){ p->waiting_for=-1; p->dl_preempted=0;
                         release(&dl_lock); return -3; }
    if(killed(p)){ p->waiting_for=-1; release(&dl_lock); return -2; }
  }

  // Resource is free — take it.
  dl_resources[rid].holder_pid = p->pid;
  if(p->holds_count < MAX_HOLDS)
    p->holds[p->holds_count++] = rid;
  p->waiting_for = -1;

  release(&dl_lock);
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
  acquire(&dl_lock);

  if(dl_resources[rid].holder_pid != p->pid){
    release(&dl_lock);
    return -1;
  }

  // Remove from holds[].
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

  release(&dl_lock);
  return 0;
}

// ── dlsetmode(int mode) ──────────────────────────────────────────────────────
// 0 = normal, 1 = aggressive.  Persistent until changed again.
uint64
sys_dlsetmode(void)
{
  int mode;
  argint(0, &mode);
  if(mode != DL_MODE_NORMAL && mode != DL_MODE_AGGRESSIVE) return -1;
  dl_mode = mode;
  printf("Deadlock mode set to: %s\n",
         mode == DL_MODE_AGGRESSIVE ? "AGGRESSIVE" : "NORMAL");
  return 0;
}

// ── dlsetresolution(int res) ─────────────────────────────────────────────────
// 0 = kill, 1 = preempt.  Persistent until changed again.
uint64
sys_dlsetresolution(void)
{
  int res;
  argint(0, &res);
  if(res != DL_RES_KILL && res != DL_RES_PREEMPT) return -1;
  dl_resolution = res;
  printf("Deadlock resolution set to: %s\n",
         res == DL_RES_KILL ? "KILL" : "PREEMPT");
  return 0;
}

// ── setpriority(int prio) ────────────────────────────────────────────────────
// Set the deadlock-scoring priority of the calling process.
// prio 0 = lowest (killed first), 9 = highest (protected).
uint64
sys_setpriority(void)
{
  int prio;
  argint(0, &prio);
  if(prio < 0 || prio > 9) return -1;
  myproc()->priority = prio;
  return 0;
}
