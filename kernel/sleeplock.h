// Long-term locks for processes
struct sleeplock {
  uint locked;        // Is the lock held?
  struct spinlock lk; // spinlock protecting this sleep lock

  // For debugging:
  char *name;         // Name of lock.
  int  pid;           // Process holding lock

  int  dl_rid;        // deadlock subsystem resource ID (-1 = exempt)
};

