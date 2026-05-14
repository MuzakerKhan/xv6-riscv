#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dltest4 -- demonstrates resource PREEMPTION mode (no process is killed).
//
// Same circular deadlock as dltest, but with resolution=PREEMPT.
// The victim process has its resources stripped and receives error code -3.
// It can then retry or exit gracefully -- it is NOT killed.
//
// Run sequence:
//   dlmode preempt     (switch to preemption mode)
//   dltest4            (run this test)
//   dlmode kill        (switch back to kill mode if desired)
//
// Expected: both processes print a message and exit cleanly. No process dies.

int
main(void)
{
  printf("=== dltest4: preemption-mode deadlock resolution ===\n");
  printf("No process will be killed. The victim loses its resources\n");
  printf("and gets error -3, then exits cleanly.\n\n");
  printf("Make sure you ran: dlmode preempt\n\n");

  int pid = fork();
  if(pid < 0){ printf("fork failed\n"); exit(1); }

  if(pid == 0){
    // CHILD: Process B
    printf("B (pid=%d): acquiring token 5...\n", getpid());
    if(dlacquire(5) < 0){ printf("B: failed\n"); exit(1); }
    printf("B (pid=%d): got token 5. Yielding...\n", getpid());
    pause(5);

    printf("B (pid=%d): requesting token 6 -- deadlock forms here\n", getpid());
    int r = dlacquire(6);
    if(r == -3){
      printf("B (pid=%d): preempted (resources stripped). Exiting gracefully.\n",
             getpid());
      // resources already stripped by kernel -- no need to release
      exit(0);
    } else if(r == -2){
      printf("B (pid=%d): killed (mode was KILL not PREEMPT -- check dlmode)\n",
             getpid());
      exit(1);
    }
    printf("B (pid=%d): got token 6. Releasing.\n", getpid());
    dlrelease(6); dlrelease(5);
    exit(0);

  } else {
    // PARENT: Process A
    printf("A (pid=%d): acquiring token 6...\n", getpid());
    if(dlacquire(6) < 0){ printf("A: failed\n"); wait(0); exit(1); }
    printf("A (pid=%d): got token 6. Yielding...\n", getpid());
    pause(5);

    printf("A (pid=%d): requesting token 5 -- deadlock forms here\n", getpid());
    int r = dlacquire(5);
    if(r == -3){
      printf("A (pid=%d): preempted (resources stripped). Exiting gracefully.\n",
             getpid());
      wait(0); exit(0);
    } else if(r == -2){
      printf("A (pid=%d): killed (mode was KILL not PREEMPT -- check dlmode)\n",
             getpid());
      dlrelease(6);
      wait(0); exit(1);
    }
    printf("A (pid=%d): got token 5. Releasing.\n", getpid());
    dlrelease(5); dlrelease(6);
    wait(0);
    printf("=== dltest4 done ===\n");
  }

  exit(0);
}
