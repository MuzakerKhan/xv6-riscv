#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dlmon -- deadlock monitor dashboard.
// Prints the current system state 10 times (one per second), then exits.
// Run it again if you want another snapshot window.
// To watch continuously: run  "dlmon"  after each dltest invocation.

int
main(void)
{
  int rounds = 10;
  for(int i = 0; i < rounds; i++){
    printf("\n--- Deadlock Monitor (round %d/%d) ---\n", i+1, rounds);
    dlstate();
    pause(10);
  }
  printf("dlmon: done.\n");
  exit(0);
}
