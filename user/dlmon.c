#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dlmon -- live deadlock dashboard.
// Refreshes every ~1 second until killed (Ctrl+A X in QEMU, or kill dlmon).
// Shows: mode, resolution, all held resources, per-process scores,
// Banker's analysis, and current deadlock status.

int
main(void)
{
  while(1){
    // ANSI clear-screen + cursor home.
    printf("\033[2J\033[H");
    printf("╔══════════════════════════════════════╗\n");
    printf("║     DEADLOCK MONITOR  (Ctrl+C quit)  ║\n");
    printf("╚══════════════════════════════════════╝\n");
    dlstate();
    pause(10);   // ~1 second (10 timer ticks)
  }
  exit(0);
}
