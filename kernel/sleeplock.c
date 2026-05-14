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
  lk->dl_rid = -1;

  extern int dl_ready;
  extern int dl_register(char*, int);
  if(dl_ready)
    lk->dl_rid = dl_register(name, 2 /* DL_TYPE_SLEEPLOCK */);
}

void
acquiresleep(struct sleeplock *lk)
{
  extern void dl_on_wait(int, int);
  extern void dl_on_acquire(int, int, int);

  acquire(&lk->lk);
  while (lk->locked) {
    // Record that this process is waiting on this sleeplock.
    if(lk->dl_rid >= 0 && myproc() && myproc()->pid > 0)
      dl_on_wait(lk->dl_rid, myproc()->pid);
    sleep(lk, &lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);

  // Record acquisition.
  if(lk->dl_rid >= 0 && myproc() && myproc()->pid > 0)
    dl_on_acquire(lk->dl_rid, myproc()->pid, 2 /* DL_TYPE_SLEEPLOCK */);
}

void
releasesleep(struct sleeplock *lk)
{
  extern void dl_on_release(int, int);
  if(lk->dl_rid >= 0 && myproc() && myproc()->pid > 0)
    dl_on_release(lk->dl_rid, myproc()->pid);

  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
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



