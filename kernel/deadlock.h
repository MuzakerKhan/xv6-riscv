#ifndef DEADLOCK_H
#define DEADLOCK_H

// total number of resource slots in our table
// first DL_TOKEN_MAX slots are for user programs (tokens)
// rest are for sleeplocks and pipes registered automatically
#define MAX_RESOURCES   256
#define MAX_HOLDS       16
#define DL_TOKEN_MAX    8

// what kind of resource is it
#define DL_TYPE_TOKEN     0
#define DL_TYPE_SPINLOCK  1
#define DL_TYPE_SLEEPLOCK 2
#define DL_TYPE_PIPE      3

// one entry in our resource table
struct dl_resource {
  int  holder_pid;  // which process holds this right now, -1 means free
  int  res_type;    // one of DL_TYPE_* above
  char res_name[16];
};

// two modes: normal just records info, aggressive does full checking
#define DL_MODE_NORMAL     0
#define DL_MODE_AGGRESSIVE 1

// two ways to fix a deadlock
#define DL_RES_KILL    0   // kill the victim process
#define DL_RES_PREEMPT 1   // take its resources but keep it alive

// weights for the scoring formula
// score = (9-priority)*W_PRIO + inverse_progress*W_PROG + holds*W_HOLD
// higher score means that process is picked as the victim first
#define W_PRIO   100
#define W_PROG    50
#define W_HOLD    20
#define PROG_CAP 10000

// how many timer ticks between automatic checks
#define DL_CHECK_TICKS  100

// global variables defined in deadlock.c
extern struct spinlock     dl_lock;
extern struct dl_resource  dl_resources[MAX_RESOURCES];
extern int                 dl_nresources;
extern int                 dl_mode;
extern int                 dl_resolution;
extern volatile int        dl_check_pending;
extern int                 dl_ready;

// function declarations
void deadlock_init(void);

// these three need dl_lock to be held before calling
int  dl_detect_locked(void);
void dl_resolve_locked(void);
int  dl_banker_safe_locked(int pid, int rid);

// called from sleeplock and pipe code when a process acquires or waits
void dl_on_acquire(int rid, int pid, int type);
void dl_on_release(int rid, int pid, int type);
void dl_on_wait(int rid, int pid);
void dl_on_unwait(int pid);

// wrapper functions for acquiring and releasing dl_lock safely
int  dl_lock_acquire(void);
void dl_lock_release(void);

// print current state to console
void dl_print_state(void);

// called from timer to run periodic detection
void dl_maybe_check(void);

// called when a process exits to free its resources
void dl_proc_cleanup(int pid);

// register a new resource in the table, returns the assigned id
int  dl_register(char *name, int type);

#endif
