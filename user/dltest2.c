#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// dltest2 -- three-process circular deadlock using real file I/O + tokens.
//
// Each process protects access to a real file using a token (as a mutex).
// The processes request each other's file-tokens in a circular order:
//
//   Process A: holds token 0 (file0), wants token 1 (file1)
//   Process B: holds token 1 (file1), wants token 2 (file2)
//   Process C: holds token 2 (file2), wants token 0 (file0)
//
// This is a 3-node cycle. The DFS detector must follow the chain
// A->B->C->A to find it. Shows that detection works beyond two processes.
// The files are real OS files -- the tokens act as mutexes protecting them,
// which is exactly how real applications use mutex locks.

static void
do_file_work(int fd, int pid)
{
  char buf[32];
  buf[0] = '0' + pid;
  buf[1] = '\n';
  write(fd, buf, 2);
}

int
main(void)
{
  printf("=== dltest2: 3-process circular deadlock (A->B->C->A) ===\n");
  printf("Tokens act as mutexes protecting real file access.\n\n");

  // Create three real files.
  int fd0 = open("dl2file0", O_RDWR | O_CREATE);
  int fd1 = open("dl2file1", O_RDWR | O_CREATE);
  int fd2 = open("dl2file2", O_RDWR | O_CREATE);
  if(fd0 < 0 || fd1 < 0 || fd2 < 0){
    printf("dltest2: could not create files\n");
    exit(1);
  }
  close(fd0); close(fd1); close(fd2);

  int pidB = fork();
  if(pidB < 0){ printf("fork failed\n"); exit(1); }

  if(pidB == 0){
    // Process B: token 1 -> token 2
    int pidC = fork();
    if(pidC < 0){ printf("fork failed\n"); exit(1); }

    if(pidC == 0){
      // Process C: token 2 -> token 0
      printf("C (pid=%d): acquiring token 2...\n", getpid());
      if(dlacquire(2) < 0){ printf("C: failed\n"); exit(1); }
      printf("C (pid=%d): got token 2. Opening dl2file2...\n", getpid());
      int fd = open("dl2file2", O_RDWR);
      if(fd >= 0){ do_file_work(fd, getpid()); close(fd); }

      pause(5);

      printf("C (pid=%d): requesting token 0 -- deadlock forms here\n", getpid());
      int r = dlacquire(0);
      if(r < 0){
        printf("C (pid=%d): deadlock victim (code %d)\n", getpid(), r);
        dlrelease(2);
        exit(1);
      }
      printf("C (pid=%d): got token 0. Releasing all.\n", getpid());
      dlrelease(0); dlrelease(2);
      exit(0);

    } else {
      // Process B
      printf("B (pid=%d): acquiring token 1...\n", getpid());
      if(dlacquire(1) < 0){ printf("B: failed\n"); wait(0); exit(1); }
      printf("B (pid=%d): got token 1. Opening dl2file1...\n", getpid());
      int fd = open("dl2file1", O_RDWR);
      if(fd >= 0){ do_file_work(fd, getpid()); close(fd); }

      pause(5);

      printf("B (pid=%d): requesting token 2 -- deadlock forms here\n", getpid());
      int r = dlacquire(2);
      if(r < 0){
        printf("B (pid=%d): deadlock victim (code %d)\n", getpid(), r);
        dlrelease(1);
        wait(0); exit(1);
      }
      printf("B (pid=%d): got token 2. Releasing all.\n", getpid());
      dlrelease(2); dlrelease(1);
      wait(0); exit(0);
    }

  } else {
    // Process A: token 0 -> token 1
    printf("A (pid=%d): acquiring token 0...\n", getpid());
    if(dlacquire(0) < 0){ printf("A: failed\n"); wait(0); exit(1); }
    printf("A (pid=%d): got token 0. Opening dl2file0...\n", getpid());
    int fd = open("dl2file0", O_RDWR);
    if(fd >= 0){ do_file_work(fd, getpid()); close(fd); }

    pause(5);

    printf("A (pid=%d): requesting token 1 -- deadlock forms here\n", getpid());
    int r = dlacquire(1);
    if(r < 0){
      printf("A (pid=%d): deadlock victim (code %d)\n", getpid(), r);
      dlrelease(0);
      wait(0); exit(1);
    }
    printf("A (pid=%d): got token 1. Releasing all.\n", getpid());
    dlrelease(1); dlrelease(0);
    wait(0);
    printf("=== dltest2 done ===\n");
  }

  exit(0);
}
