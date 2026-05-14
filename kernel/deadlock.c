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

// Per-CPU flag: set whenever this CPU holds dl_lock (or is in the process of
// acquiring it).  Checked by spinlock hooks to prevent dl_lock double-acquire
// panics (e.g. printf inside dl_print_state acquires console lock → hook fires
// → would try to acquire dl_lock again → panic).
static int dl_cpu_busy[NCPU];

extern struct proc proc[];

// ─── dl_lock wrappers ─────────────────────────────────────────────────────────
// Always use these instead of bare acquire/release on dl_lock.

int
dl_lock_acquire(void)
{
  // cpuid() reads the tp register which is CPU-local and never changes —
  // safe to call without push_off.
  int cpu = r_tp();
  if(dl_cpu_busy[cpu]) return 0;   // already held by this CPU — skip
  dl_cpu_busy[cpu] = 1;
  acquire(&dl_lock);
  return 1;
}

void
dl_lock_release(void)
{
  int cpu = r_tp();
  release(&dl_lock);
  dl_cpu_busy[cpu] = 0;
}

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
  for(int i = 0; i < DL_TOKEN_MAX; i++){
    dl_resources[i].name[0] = 'T';
    dl_resources[i].name[1] = '0' + i;
    dl_resources[i].name[2] = '\0';
  }
  dl_ready = 1;
}

// ─── REGISTER ────────────────────────────────────────────────────────────────
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

// ─── TABLE HELPERS ────────────────────────────────────────────────────────────
// Spinlocks are acquired/released in nanoseconds and processes never sleep while
// holding them — so they cannot cause inter-process deadlock.  We track their
// holder_pid for completeness but do NOT add them to per-process holds[] (which
// is used for deadlock scoring and resolution).
static void
update_acquire(int rid, int pid, int type)
{
  dl_resources[rid].holder_pid = pid;
  if(type == DL_TYPE_SPINLOCK) return;   // don't pollute holds[] with spinlocks

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
update_release(int rid, int pid, int type)
{
  dl_resources[rid].holder_pid = -1;
  if(type == DL_TYPE_SPINLOCK) return;

  for(int i = 0; i < NPROC; i++){
    if(proc[i].pid != pid) continue;
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

// ─── HOOK ENTRY POINTS ───────────────────────────────────────────────────────
// Called from spinlock/sleeplock/pipe hooks.  dl_cpu_busy check prevents
// re-entry when this CPU is already inside the dl subsystem.

void
dl_on_acquire(int rid, int pid, int type)
{
  if(!dl_ready || rid < 0 || pid <= 0) return;
  if(dl_cpu_busy[r_tp()]) return;    // this CPU is already doing dl work
  if(!dl_lock_acquire()) return;

  update_acquire(rid, pid, type);

  if(dl_mode == DL_MODE_AGGRESSIVE && dl_detect_locked())
    dl_resolve_locked();             // releases dl_lock internally
  else
    dl_lock_release();
}

void
dl_on_release(int rid, int pid, int type)
{
  if(!dl_ready || rid < 0 || pid <= 0) return;
  if(dl_cpu_busy[r_tp()]) return;
  if(!dl_lock_acquire()) return;

  update_release(rid, pid, type);
  dl_lock_release();
}

void
dl_on_wait(int rid, int pid)
{
  if(!dl_ready || rid < 0 || pid <= 0) return;
  if(dl_cpu_busy[r_tp()]) return;
  if(!dl_lock_acquire()) return;

  for(int i = 0; i < NPROC; i++){
    if(proc[i].pid == pid){ proc[i].waiting_for = rid; break; }
  }
  dl_lock_release();
}

void
dl_on_unwait(int pid)
{
  if(!dl_ready || pid <= 0) return;
  if(dl_cpu_busy[r_tp()]) return;
  if(!dl_lock_acquire()) return;

  for(int i = 0; i < NPROC; i++){
    if(proc[i].pid == pid){ proc[i].waiting_for = -1; break; }
  }
  dl_lock_release();
}

// ─── DETECTION ───────────────────────────────────────────────────────────────
// Caller must hold dl_lock (via dl_lock_acquire).

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

// ─── BANKER'S ────────────────────────────────────────────────────────────────
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

  dl_resources[rid].holder_pid = old;
  p->waiting_for = owait;
  if(added){ p->holds_count--; p->holds[p->holds_count]=-1; }
  return safe;
}

// ─── SCORING ──────────────────────────────────────────────────────────────────
static int
kill_score(struct proc *p)
{
  int prio_part = (9 - p->priority) * DL_W_PRIO;

  uint64 ticks = p->cpu_ticks;
  if(ticks > DL_PROG_CAP) ticks = DL_PROG_CAP;
  int prog_inv  = (int)((DL_PROG_CAP - ticks) * 100 / DL_PROG_CAP);
  int prog_part = prog_inv * DL_W_PROG;

  int hold_part = p->holds_count * DL_W_HOLD;

  return prio_part + prog_part + hold_part;
}

// ─── RESOLUTION ───────────────────────────────────────────────────────────────
// Must be called with dl_lock held (via dl_lock_acquire).
// Releases dl_lock before calling kkill to avoid lock-order cycle with wait_lock.
void
dl_resolve_locked(void)
{
  int best_pid = -1, best_score = -1;

  for(int i = 0; i < NPROC; i++){
    if(proc[i].state == UNUSED || proc[i].pid <= 0) continue;
    if(proc[i].waiting_for < 0) continue;
    int s = kill_score(&proc[i]);
    if(s > best_score){ best_score = s; best_pid = proc[i].pid; }
  }

  if(best_pid <= 0){ dl_lock_release(); return; }

  // Free all resources held by victim and wake sleepers.
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

  int mode = dl_resolution;
  dl_lock_release();   // MUST release before kkill (kkill acquires wait_lock)

  if(mode == DL_RES_KILL)
    kkill(best_pid);

  printf("DEADLOCK RESOLVED [%s]: victim pid=%d score=%d\n",
         mode == DL_RES_KILL ? "KILL" : "PREEMPT", best_pid, best_score);
}

// ─── CLEANUP ─────────────────────────────────────────────────────────────────
void
dl_proc_cleanup(int pid)
{
  if(!dl_ready || pid <= 0) return;
  if(!dl_lock_acquire()) return;
  for(int i = 0; i < dl_nresources; i++){
    if(dl_resources[i].holder_pid == pid){
      dl_resources[i].holder_pid = -1;
      wakeup(&dl_resources[i]);
    }
  }
  dl_lock_release();
}

// ─── PERIODIC CHECK ───────────────────────────────────────────────────────────
void
dl_maybe_check(void)
{
  if(!dl_ready || !dl_check_pending) return;
  dl_check_pending = 0;
  if(!dl_lock_acquire()) return;
  if(dl_detect_locked())
    dl_resolve_locked();
  else
    dl_lock_release();
}

// ─── PRINT STATE ─────────────────────────────────────────────────────────────
void
dl_print_state(void)
{
  dl_maybe_check();
  if(!dl_lock_acquire()) return;

  printf("\n========== DEADLOCK SUBSYSTEM ==========\n");
  printf("Mode: %s   Resolution: %s\n",
         dl_mode == DL_MODE_AGGRESSIVE ? "AGGRESSIVE" : "NORMAL",
         dl_resolution == DL_RES_KILL  ? "KILL"       : "PREEMPT");

  printf("\nResources held (%d registered):\n", dl_nresources);
  for(int i = 0; i < dl_nresources; i++){
    if(dl_resources[i].holder_pid < 0) continue;
    char *tname = "?";
    switch(dl_resources[i].type){
      case DL_TYPE_TOKEN:     tname = "TOKEN";    break;
      case DL_TYPE_SPINLOCK:  tname = "SPINLK";   break;
      case DL_TYPE_SLEEPLOCK: tname = "SLEEPLK";  break;
      case DL_TYPE_PIPE:      tname = "PIPE";      break;
    }
    printf("  [%d] %s %s  pid=%d\n",
           i, tname, dl_resources[i].name,
           dl_resources[i].holder_pid);
  }

  printf("\nProcesses:\n");
  for(int i = 0; i < NPROC; i++){
    if(proc[i].state == UNUSED || proc[i].pid <= 0) continue;
    printf("  pid=%d %s  prio=%d  cpu=%lu  holds=%d  wait=%d  score=%d\n",
           proc[i].pid, proc[i].name,
           proc[i].priority,
           (unsigned long)proc[i].cpu_ticks,
           proc[i].holds_count,
           proc[i].waiting_for,
           kill_score(&proc[i]));
  }

  // Banker's for waiting processes.
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
      printf("  pid=%d requesting rid=%d -> %s\n",
             proc[i].pid, rid,
             dl_banker_safe_locked(proc[i].pid, rid) ? "SAFE" : "UNSAFE");
    }
  }

  if(dl_detect_locked()){
    printf("\n*** DEADLOCK DETECTED ***\n");
    dl_resolve_locked();
  } else {
    printf("\nStatus: no deadlock\n");
    printf("==========================================\n\n");
    dl_lock_release();
  }
}
