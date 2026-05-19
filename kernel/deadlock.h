#ifndef DEADLOCK_H
#define DEADLOCK_H

// total number of resource slots in our table
// first DL_TOKEN_MAX slots are for user programs (tokens), Virtual resources that user programs can acquire and release, used for testing
// rest are for sleeplocks and pipes registered automatically
#define MAX_RESOURCES   256
#define MAX_HOLDS       16
#define DL_TOKEN_MAX    8
//type of resource.
#define DL_TYPE_TOKEN     0
#define DL_TYPE_SPINLOCK  1
#define DL_TYPE_SLEEPLOCK 2
#define DL_TYPE_PIPE      3
// one entry in our resource table
struct dl_resource {
  int  holder_pid;  // which process holds this resource right now and -1 means free
  int  res_type;    // one of type discussed above
  char res_name[16]; //name of resource
};

// two modes: normal just records info, aggressive does full checking
#define DL_MODE_NORMAL     0
#define DL_MODE_AGGRESSIVE 1
// the two ways to fix a deadlock
#define DL_RES_KILL    0   // kill the victim process
#define DL_RES_PREEMPT 1   // take its resources but keep it alive

// weights for scoring formula
// score = (9-priority)*W_PRIO + inverse_progress*W_PROG + holds*W_HOLD
// higher score means that process is picked as the victim first
// W = weight
#define W_PRIO   100 
#define W_PROG    50
#define W_HOLD    20
#define PROG_CAP 10000 
// how many timer ticks between automatic checks
#define DL_CHECK_TICKS  100 // 1 sec
// global variables defined in deadlock.c
//extern: means these are defined in the .c file but we want to use them in other files that include this header
extern struct spinlock     dl_lock; // lock to protect access to the resource table and related variables
extern struct dl_resource  dl_resources[MAX_RESOURCES]; // table of all resources
extern int                 dl_nresources; // number of resources in the table
extern int                 dl_mode; // current mode (normal or aggressive)
extern int                 dl_resolution; // current resolution strategy
extern volatile int        dl_check_pending; // flag indicating if a check is pending
extern int                 dl_ready; // flag indicating if the deadlock system is ready

//there is only void in the parameter because the function does not take any arguments. safty.
// function declarations
void deadlock_init(void); // initialize the deadlock detection system, called from main.c at startup
// these three need dl_lock to be held before calling
int  dl_detect_locked(void); // return true if deadlock detected and false if not.
void dl_resolve_locked(void); // called when detection fucntion return true and resolve the deadlock by selected method
int  dl_banker_safe_locked(int pid, int rid); //return true if it's safe for process to aquire the  resouce without deadlock.

// called from sleeplock and pipe code when a process acquires or waits
void dl_on_acquire(int rid, int pid, int type); // called when a process releases a resource
void dl_on_release(int rid, int pid, int type); // called when a process waits for a resource
void dl_on_wait(int rid, int pid); // called when a process waits for a resource
void dl_on_unwait(int pid); // called when a process stops waiting for a resource

// wrapper functions for acquiring and releasing dl_lock safely
int  dl_lock_acquire(void); // acquire the deadlock lock
void dl_lock_release(void); // release the deadlock lock
// print current state to console
void dl_print_state(void);
// called from timer to run periodic detection
void dl_maybe_check(void);
// called when a process exits to free its resources
void dl_proc_cleanup(int pid);
// register a new resource in the table, returns the assigned id
int  dl_register(char *name, int type);

#endif
