#ifndef DEADLOCK_H
#define DEADLOCK_H

// ─── resource table ───────────────────────────────────────────────────────────
// Resources 0..DL_TOKEN_MAX-1 are explicit user tokens (dlacquire/dlrelease).
// Resources DL_TOKEN_MAX..MAX_RESOURCES-1 are auto-registered kernel objects
// (spinlocks, sleeplocks, pipes).

#define MAX_RESOURCES   256
#define MAX_HOLDS       16
#define DL_TOKEN_MAX    8       // explicit user-space tokens

// Resource types — stored in resource_table[i].type
#define DL_TYPE_TOKEN     0
#define DL_TYPE_SPINLOCK  1
#define DL_TYPE_SLEEPLOCK 2
#define DL_TYPE_PIPE      3

struct dl_resource {
  int  holder_pid;   // -1 = free
  int  type;         // DL_TYPE_*
  char name[16];
};

// ─── operation modes ─────────────────────────────────────────────────────────
#define DL_MODE_NORMAL     0   // record-only for kernel locks; full mgmt for tokens
#define DL_MODE_AGGRESSIVE 1   // full deadlock management on ALL resource accesses

// ─── resolution modes ────────────────────────────────────────────────────────
#define DL_RES_KILL    0   // terminate victim process
#define DL_RES_PREEMPT 1   // strip victim's resources, let it continue

// ─── scoring weights ─────────────────────────────────────────────────────────
// kill_score = (9-priority)*W_PRIO + inv_progress*W_PROG + holds_count*W_HOLD
// Higher kill_score → chosen as victim first.
#define DL_W_PRIO   100
#define DL_W_PROG    50
#define DL_W_HOLD    20
#define DL_PROG_CAP 10000   // cpu_ticks cap for progress normalisation

// ─── periodic detection ───────────────────────────────────────────────────────
#define DL_CHECK_TICKS  100  // run detection every N timer ticks

// ─── globals (defined in deadlock.c) ─────────────────────────────────────────
extern struct spinlock     dl_lock;
extern struct dl_resource  dl_resources[MAX_RESOURCES];
extern int                 dl_nresources;     // next free slot
extern int                 dl_mode;           // DL_MODE_*
extern int                 dl_resolution;     // DL_RES_*
extern volatile int        dl_check_pending;  // set by clockintr
extern int                 dl_ready;          // 1 after deadlock_init

// ─── API ──────────────────────────────────────────────────────────────────────
void deadlock_init(void);

// Internal helpers (caller must hold dl_lock).
int  dl_detect_locked(void);
void dl_resolve_locked(void);        // releases dl_lock before calling kkill
int  dl_banker_safe_locked(int pid, int rid);

// Called from kernel lock hooks (no dl_lock needed — uses hook guard).
void dl_on_acquire(int rid, int pid, int type);
void dl_on_release(int rid, int pid);
void dl_on_wait(int rid, int pid);   // called when process is about to sleep
void dl_on_unwait(int pid);          // called when process wakes up

// Called from sysdeadlock / user-facing code.
void dl_print_state(void);
void dl_maybe_check(void);           // run pending periodic detection

// Called from freeproc.
void dl_proc_cleanup(int pid);

// Register a resource slot, returns its ID (or -1 if full).
int  dl_register(char *name, int type);

#endif
