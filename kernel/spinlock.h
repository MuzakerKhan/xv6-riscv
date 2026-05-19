// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.

  int resource_id;   // id in the deadlock resource table, always -1 for spinlocks (not tracked)
};

