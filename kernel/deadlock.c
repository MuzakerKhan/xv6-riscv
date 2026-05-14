#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "deadlock.h"

// ─── globals ──────────────────────────────────────────────────────────────────
struct spinlock    dl_lock;
struct dl_resource dl_resources[MAX_RESOURCES];
int                dl_nresources = DL_TOKEN_MAX;  // tokens 0..7 pre-reserved
int                dl_mode       = DL_MODE_NORMAL;
int                dl_resolution = DL_RES_KILL;
volatile int       dl_check_pending = 0;
int                dl_ready      = 0;

// Per-CPU recursion guard so the dl_lock acquire inside a hook can never
// re-enter the hook for dl_lock itself.
static int dl_hook_active[NCPU];

extern struct proc proc[];

// ─── INIT ────────────────────────────────────────────────────────────────────
void
deadlock_init(void)
{
  initlock(&dl_lock, "dl_lock");
  for(int i = 0; i < MAX_RESOURCES; i++){
    dl_resources[i].holder_pid = -1;
    dl_resources[i].type       = DL_TYPE_TOKEN;
    dl_resources[i].name[0]    = '\0';
  }
  // Label the user-visible token slots.
  for(int i = 0; i < DL_TOKEN_MAX; i++){
    dl_resources[i].name[0] = 'T';
    dl_resources[i].name[1] = '0' + i;
    dl_resources[i].name[2] = '\0';
  }
  dl_ready = 1;
}

// ─── REGISTER ────────────────────────────────────────────────────────────────
// Allocate a new resource slot. Called from initlock / initsleeplock / pipealloc.
// No dl_lock needed: only ever called single-threaded during kernel init,
// and dl_nresources is only written here.
int
dl_register(char *name, int type)
{
  if(!dl_ready) return -1;
  if(dl_nresources >= MAX_RESOURCES) return -1;

  int id = dl_nresources++;
  dl_resources[id].holder_pid = -1;
  dl_resources[id].type       = type;
  int i;
  for(i = 0; i < 15 && name[i]; i++)
    dl_resources[id].name[i] = name[i];
  dl_resources[id].name[i] = '\0';
  return id;
}

// ─── HOOK HELPERS ─────────────────────────────────────────────────────────────
// These are called from acquire/acquiresleep/piperead hotpaths.
// They protect shared tables with dl_lock but guard against re-entry.

static void
update_acquire(int rid, int pid)
{
  // Update resource holder.
  dl_resources[rid].holder_pid = pid;

  // Add to process hold list.
  for(int i = 0; i < NPROC; i++){
    if(proc[i].pid != pid) continue;
    if(proc[i].holds_count < MAX_HOLDS)
      proc[i].holds[proc[i].holds_count++] = rid;
    proc[i].waiting_for  = -1;
    proc[i].dl_preempted = 0;
    break;
  }
}

static void
update_release(int rid, int pid)
{
  dl_resources[rid].holder_pid = -1;

  for(int i = 0; i < NPROC; i++){
    if(proc[i].pid != pid) continue;
    // Remove rid from holds[].
    for(int k = 0; k < proc[i].holds_count; k++){
      if(proc[i].holds[k] != rid) continue;
      for(int j = k; j < proc[i].holds_count - 1; j++)
        proc[i].holds[j] = proc[i].holds[j+1];
      proc[i].holds_count--;
      proc[i].holds[proc[i].holds_count] = -1;
      break;
    }
    break;
  }
}

void
dl_on_acquire(int rid, int pid, int type)
{
  if(!dl_ready || rid < 0 || pid <= 0) return;

  int cpu = cpuid();
  if(dl_hook_active[cpu]) return;
  dl_hook_active[cpu] = 1;

  acquire(&dl_lock);
  update_acquire(rid, pid);

  // In aggressive mode, check for a newly formed deadlock right now.
  if(dl_mode == DL_MODE_AGGRESSIVE && dl_detect_locked())
    dl_resolve_locked();  // releases dl_lock internally
  else
    release(&dl_lock);

  dl_hook_active[cpu] = 0;
}

void
dl_on_release(int rid, int pid)
{
  if(!dl_ready || rid < 0 || pid <= 0) return;

  int cpu = cpuid();
  if(dl_hook_active[cpu]) return;
  dl_hook_active[cpu] = 1;

  acquire(&dl_lock);
  update_release(rid, pid);
  release(&dl_lock);

  dl_hook_active[cpu] = 0;
}

void
dl_on_wait(int rid, int pid)
{
  if(!dl_ready || rid < 0 || pid <= 0) return;

  int cpu = cpuid();
  if(dl_hook_active[cpu]) return;
  dl_hook_active[cpu] = 1;

  acquire(&dl_lock);
  for(int i = 0; i < NPROC; i++){
    if(proc[i].pid == pid){
      proc[i].waiting_for = rid;
      break;
    }
  }
  release(&dl_lock);

  dl_hook_active[cpu] = 0;
}

void
dl_on_unwait(int pid)
{
  if(!dl_ready || pid <= 0) return;

  int cpu = cpuid();
  if(dl_hook_active[cpu]) return;
  dl_hook_active[cpu] = 1;

  acquire(&dl_lock);
  for(int i = 0; i < NPROC; i++){
    if(proc[i].pid == pid){
      proc[i].waiting_for = -1;
      break;
    }
  }
  release(&dl_lock);

  dl_hook_active[cpu] = 0;
}

// ─── DETECTION ───────────────────────────────────────────────────────────────
// DFS on the wait-for graph.  Caller must hold dl_lock.

static int vis[NPROC], stk[NPROC];

static int
pid_idx(int pid)
{
  for(int i = 0; i < NPROC; i++)
    if(proc[i].pid == pid && proc[i].state != UNUSED)
      return i;
  return -1;
}

static int
dfs(int idx)
{
  vis[idx] = stk[idx] = 1;

  int wfor = proc[idx].waiting_for;
  if(wfor < 0 || wfor >= dl_nresources){ stk[idx]=0; return 0; }

  int holder = dl_resources[wfor].holder_pid;
  if(holder <= 0){ stk[idx]=0; return 0; }

  int nxt = pid_idx(holder);
  if(nxt < 0){ stk[idx]=0; return 0; }

  if(stk[nxt]) return 1;
  if(!vis[nxt] && dfs(nxt)) return 1;

  stk[idx] = 0;
  return 0;
}

int
dl_detect_locked(void)
{
  for(int i = 0; i < NPROC; i++) vis[i] = stk[i] = 0;
  for(int i = 0; i < NPROC; i++)
    if(proc[i].state != UNUSED && proc[i].pid > 0 && !vis[i])
      if(dfs(i)) return 1;
  return 0;
}

// ─── BANKER'S ALGORITHM ───────────────────────────────────────────────────────
// Single-instance resources: safe ↔ no cycle after hypothetical grant.
// Caller must hold dl_lock.
int
dl_banker_safe_locked(int pid, int rid)
{
  int old = dl_resources[rid].holder_pid;
  dl_resources[rid].holder_pid = pid;

  struct proc *p = 0;
  for(int i = 0; i < NPROC; i++)
    if(proc[i].pid == pid){ p = &proc[i]; break; }
  if(!p){ dl_resources[rid].holder_pid = old; return 0; }

  int added = 0;
  if(p->holds_count < MAX_HOLDS){ p->holds[p->holds_count++] = rid; added=1; }
  int owait = p->waiting_for;
  p->waiting_for = -1;

  int safe = !dl_detect_locked();

  // Undo.
  dl_resources[rid].holder_pid = old;
  p->waiting_for = owait;
  if(added){ p->holds_count--; p->holds[p->holds_count]=-1; }
  return safe;
}

// ─── SCORING ──────────────────────────────────────────────────────────────────
// Higher kill_score → victim chosen first.
static int
kill_score(struct proc *p)
{
  // Priority component: lower process priority → higher kill score.
  int prio_part = (9 - p->priority) * DL_W_PRIO;

  // Progress component: less CPU time → higher kill score.
  uint64 ticks = p->cpu_ticks;
  if(ticks > DL_PROG_CAP) ticks = DL_PROG_CAP;
  int prog_inv  = (int)((DL_PROG_CAP - ticks) * 100 / DL_PROG_CAP);
  int prog_part = prog_inv * DL_W_PROG;

  // Resource-hold component: more resources → more entangled.
  int hold_part = p->holds_count * DL_W_HOLD;

  return prio_part + prog_part + hold_part;
}

// ─── RESOLUTION ───────────────────────────────────────────────────────────────
// Finds victim, frees its resources, then either kills it or preempts it.
// IMPORTANT: releases dl_lock before calling kkill to avoid lock-order cycle
// with wait_lock (which kkill acquires internally).
void
dl_resolve_locked(void)
{
  int best_pid = -1, best_score = -1;

  for(int i = 0; i < NPROC; i++){
    if(proc[i].state == UNUSED || proc[i].pid <= 0) continue;
    if(proc[i].waiting_for < 0) continue;  // not blocked on a dl resource
    int s = kill_score(&proc[i]);
    if(s > best_score){ best_score = s; best_pid = proc[i].pid; }
  }

  if(best_pid <= 0){ release(&dl_lock); return; }

  printf("\nDEADLOCK RESOLVER [%s]: victim=pid%d score=%d\n",
         dl_resolution == DL_RES_KILL ? "KILL" : "PREEMPT",
         best_pid, best_score);

  // Free all resources held by the victim.
  for(int i = 0; i < NPROC; i++){
    if(proc[i].pid != best_pid) continue;
    for(int k = 0; k < proc[i].holds_count; k++){
      int rid = proc[i].holds[k];
      if(rid >= 0 && rid < dl_nresources){
        dl_resources[rid].holder_pid = -1;
        wakeup(&dl_resources[rid]);
      }
    }
    proc[i].holds_count  = 0;
    proc[i].waiting_for  = -1;
    if(dl_resolution == DL_RES_PREEMPT)
      proc[i].dl_preempted = 1;
    break;
  }

  // Release dl_lock BEFORE kkill to avoid kkill→wait_lock→dl_lock cycle.
  release(&dl_lock);

  if(dl_resolution == DL_RES_KILL)
    kkill(best_pid);
  // PREEMPT: process wakes up, sees dl_preempted=1, returns error to user.
}

// ─── CLEANUP ─────────────────────────────────────────────────────────────────
// Called from freeproc; no dl_lock held by caller.
void
dl_proc_cleanup(int pid)
{
  if(!dl_ready || pid <= 0) return;
  acquire(&dl_lock);
  for(int i = 0; i < dl_nresources; i++){
    if(dl_resources[i].holder_pid == pid){
      dl_resources[i].holder_pid = -1;
      wakeup(&dl_resources[i]);
    }
  }
  release(&dl_lock);
}

// ─── PERIODIC CHECK ───────────────────────────────────────────────────────────
void
dl_maybe_check(void)
{
  if(!dl_ready || !dl_check_pending) return;
  dl_check_pending = 0;

  acquire(&dl_lock);
  if(dl_detect_locked())
    dl_resolve_locked();   // releases dl_lock
  else
    release(&dl_lock);
}

// ─── PRINT STATE ─────────────────────────────────────────────────────────────
void
dl_print_state(void)
{
  // Run any pending periodic check first.
  dl_maybe_check();

  acquire(&dl_lock);

  printf("\n========== DEADLOCK SUBSYSTEM ==========\n");
  printf("Mode: %s   Resolution: %s\n",
         dl_mode == DL_MODE_AGGRESSIVE ? "AGGRESSIVE" : "NORMAL",
         dl_resolution == DL_RES_KILL  ? "KILL"       : "PREEMPT");

  // Resources
  printf("\nResources (%d registered):\n", dl_nresources);
  for(int i = 0; i < dl_nresources; i++){
    if(dl_resources[i].holder_pid < 0) continue;
    char *tname = "?";
    switch(dl_resources[i].type){
      case DL_TYPE_TOKEN:    tname="TOKEN";    break;
      case DL_TYPE_SPINLOCK: tname="SPINLOCK"; break;
      case DL_TYPE_SLEEPLOCK:tname="SLEEPLCK"; break;
      case DL_TYPE_PIPE:     tname="PIPE";     break;
    }
    printf("  [%3d] %-8s %-12s  held_by=pid%d\n",
           i, tname, dl_resources[i].name,
           dl_resources[i].holder_pid);
  }

  // Processes
  printf("\nProcesses:\n");
  for(int i = 0; i < NPROC; i++){
    if(proc[i].state == UNUSED || proc[i].pid <= 0) continue;
    printf("  pid=%-3d %-10s  prio=%d  cpu=%llu  holds=%d  wait=%d  score=%d\n",
           proc[i].pid, proc[i].name,
           proc[i].priority,
           (unsigned long long)proc[i].cpu_ticks,
           proc[i].holds_count,
           proc[i].waiting_for,
           kill_score(&proc[i]));
  }

  // Banker's for all waiting processes.
  int any_wait = 0;
  for(int i = 0; i < NPROC; i++)
    if(proc[i].state != UNUSED && proc[i].pid > 0 && proc[i].waiting_for >= 0)
      { any_wait = 1; break; }

  if(any_wait){
    printf("\nBanker's analysis:\n");
    for(int i = 0; i < NPROC; i++){
      if(proc[i].state == UNUSED || proc[i].pid <= 0) continue;
      int rid = proc[i].waiting_for;
      if(rid < 0) continue;
      int safe = dl_banker_safe_locked(proc[i].pid, rid);
      printf("  pid%d requesting rid=%d -> %s\n",
             proc[i].pid, rid, safe ? "SAFE" : "UNSAFE");
    }
  }

  // Detection result.
  if(dl_detect_locked()){
    printf("\n*** DEADLOCK DETECTED ***\n");
    dl_resolve_locked();   // releases dl_lock
  } else {
    printf("\nStatus: no deadlock\n");
    printf("==========================================\n\n");
    release(&dl_lock);
  }
}
