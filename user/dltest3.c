#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dltest3 -- demonstrates the priority-based scoring system.
//
// Two processes create a deadlock. One is given HIGH priority (9),
// the other LOW priority (0). The system must always kill the low-priority
// process regardless of which process detected the deadlock first.
//
// Run this after: dlmode kill
// Expected output: the low-priority process is always the victim.

int
main(void)
{
  printf("=== dltest3: priority-based victim selection ===\n");
  printf("High-priority process (prio=9) should always survive.\n");
  printf("Low-priority process  (prio=0) should always be killed.\n\n");

  int pid = fork();
  if(pid < 0){ printf("fork failed\n"); exit(1); }

  if(pid == 0){
    // CHILD: set LOW priority -- should be the victim
    setpriority(0);
    printf("LOW  (pid=%d prio=0): acquiring token 3...\n", getpid());
    if(dlacquire(3) < 0){ printf("LOW: failed to get token 3\n"); exit(1); }
    printf("LOW  (pid=%d): got token 3. Yielding...\n", getpid());
    pause(5);

    printf("LOW  (pid=%d): requesting token 4 -- deadlock forms here\n", getpid());
    int r = dlacquire(4);
    if(r < 0){
      printf("LOW  (pid=%d): killed as expected (code %d). System working correctly.\n",
             getpid(), r);
      dlrelease(3);
      exit(1);
    }
    printf("LOW  (pid=%d): WARNING -- survived when it should have been killed!\n",
           getpid());
    dlrelease(4); dlrelease(3);
    exit(0);

  } else {
    // PARENT: set HIGH priority -- should always survive
    setpriority(9);
    printf("HIGH (pid=%d prio=9): acquiring token 4...\n", getpid());
    if(dlacquire(4) < 0){ printf("HIGH: failed to get token 4\n"); wait(0); exit(1); }
    printf("HIGH (pid=%d): got token 4. Yielding...\n", getpid());
    pause(5);

    printf("HIGH (pid=%d): requesting token 3 -- deadlock forms here\n", getpid());
    int r = dlacquire(3);
    if(r < 0){
      printf("HIGH (pid=%d): killed -- scoring system has a bug! (code %d)\n",
             getpid(), r);
      dlrelease(4);
      wait(0); exit(1);
    }
    printf("HIGH (pid=%d): survived as expected. Priority scoring is correct.\n",
           getpid());
    dlrelease(3); dlrelease(4);
    wait(0);
    printf("=== dltest3 done ===\n");
  }

  exit(0);
}
