// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

void
initsleeplock(struct sleeplock *lk, char *name)
{
  initlock(&lk->lk, "sleep lock");
  lk->name   = name;
  lk->locked = 0;
  lk->pid    = 0;
  lk->dl_rid = -1; // start as unregistered

  // register this sleeplock in the deadlock resource table if the system is ready
  // dl_ready is 0 until deadlock_init() runs in main.c
  // sleeplocks created before that wont be registered (which is ok, they are early kernel ones)
  extern int dl_ready;
  extern int dl_register(char*, int);
  if(dl_ready)
    lk->dl_rid = dl_register(name, 2); // 2 = DL_TYPE_SLEEPLOCK
}

void
acquiresleep(struct sleeplock *lk)
{
  // declare these as extern so we can call into deadlock.c without including full header
  extern void dl_on_wait(int, int);
  extern void dl_on_acquire(int, int, int);

  acquire(&lk->lk);
  while(lk->locked){
    // before sleeping, tell the deadlock system this process is waiting for this sleeplock
    // this creates the waiting edge in the wait-for graph
    if(lk->dl_rid >= 0 && myproc() && myproc()->pid > 0)
      dl_on_wait(lk->dl_rid, myproc()->pid);
    sleep(lk, &lk->lk); // this sleeps until woken by releasesleep
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);

  // now we have the lock, tell the deadlock system we acquired it
  if(lk->dl_rid >= 0 && myproc() && myproc()->pid > 0)
    dl_on_acquire(lk->dl_rid, myproc()->pid, 2); // 2 = DL_TYPE_SLEEPLOCK
}

void
releasesleep(struct sleeplock *lk)
{
  extern void dl_on_release(int, int, int);

  // tell deadlock system we are releasing before we actually release
  if(lk->dl_rid >= 0 && myproc() && myproc()->pid > 0)
    dl_on_release(lk->dl_rid, myproc()->pid, 2); // 2 = DL_TYPE_SLEEPLOCK

  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk); // wake up anyone waiting for this sleeplock
  release(&lk->lk);
}

int
holdingsleep(struct sleeplock *lk)
{
  int r;

  acquire(&lk->lk);
  r = lk->locked && (lk->pid == myproc()->pid);
  release(&lk->lk);
  return r;
}
