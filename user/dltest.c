#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dltest -- create a real circular-wait deadlock between two processes.
//
// Process A: grabs token 0, then tries token 1.
// Process B: grabs token 1, then tries token 0.
// The kernel detects the cycle and resolves it according to the current mode.
//
// Run "dlmode aggressive" first for immediate detection on every block.
// Run "dlmon" in a second QEMU window to watch the live graph.

int
main(void)
{
  printf("=== dltest: two-process circular-wait deadlock ===\n");
  printf("A holds token0, wants token1.\n");
  printf("B holds token1, wants token0.\n\n");

  int pid = fork();
  if(pid < 0){ printf("fork failed\n"); exit(1); }

  if(pid == 0){
    // ── CHILD: Process B ──────────────────────────────────────────────
    printf("B (pid=%d): acquiring token 1...\n", getpid());
    if(dlacquire(1) < 0){
      printf("B: failed to acquire token 1 (victim?)\n");
      exit(1);
    }
    printf("B (pid=%d): got token 1. Yielding...\n", getpid());
    pause(5);   // let A also get token 0

    printf("B (pid=%d): trying token 0 — deadlock forms here\n", getpid());
    int r = dlacquire(0);
    if(r == -2){
      printf("B (pid=%d): killed by resolver.\n", getpid());
      dlrelease(1);
      exit(1);
    } else if(r == -3){
      printf("B (pid=%d): preempted (resources stripped). Retrying later.\n", getpid());
      dlrelease(1);
      exit(0);
    }
    printf("B (pid=%d): got token 0. Releasing.\n", getpid());
    dlrelease(0);
    dlrelease(1);
    exit(0);

  } else {
    // ── PARENT: Process A ─────────────────────────────────────────────
    printf("A (pid=%d): acquiring token 0...\n", getpid());
    if(dlacquire(0) < 0){
      printf("A: failed to acquire token 0 (victim?)\n");
      wait(0); exit(1);
    }
    printf("A (pid=%d): got token 0. Yielding...\n", getpid());
    pause(5);

    printf("A (pid=%d): trying token 1 — deadlock forms here\n", getpid());
    int r = dlacquire(1);
    if(r == -2){
      printf("A (pid=%d): killed by resolver.\n", getpid());
      dlrelease(0);
      wait(0); exit(1);
    } else if(r == -3){
      printf("A (pid=%d): preempted. Retrying later.\n", getpid());
      dlrelease(0);
      wait(0); exit(0);
    }
    printf("A (pid=%d): got token 1. Releasing.\n", getpid());
    dlrelease(1);
    dlrelease(0);
    wait(0);
    printf("=== dltest done ===\n");
  }

  exit(0);
}
