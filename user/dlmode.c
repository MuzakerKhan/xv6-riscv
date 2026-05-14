#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dlmode -- switch deadlock subsystem modes from the xv6 shell.
//
// Usage:
//   dlmode normal          -- record-only for kernel locks (safe, low overhead)
//   dlmode aggressive      -- full deadlock management on every resource access
//   dlmode kill            -- resolution: terminate the victim
//   dlmode preempt         -- resolution: strip victim's resources, keep it alive
//   dlmode prio <0-9>      -- set this process's deadlock priority
//   dlmode status          -- print current mode (same as dlmon but one-shot)

static void
usage(void)
{
  printf("Usage:\n");
  printf("  dlmode normal|aggressive      -- set detection mode\n");
  printf("  dlmode kill|preempt           -- set resolution mode\n");
  printf("  dlmode prio <0-9>             -- set this process priority (0=low,9=high)\n");
  printf("  dlmode status                 -- print system state\n");
}

static int
streq(const char *a, const char *b)
{
  while(*a && *b && *a == *b){ a++; b++; }
  return *a == 0 && *b == 0;
}

int
main(int argc, char *argv[])
{
  if(argc < 2){ usage(); exit(1); }

  if(streq(argv[1], "normal")){
    dlsetmode(0);

  } else if(streq(argv[1], "aggressive")){
    dlsetmode(1);

  } else if(streq(argv[1], "kill")){
    dlsetresolution(0);

  } else if(streq(argv[1], "preempt")){
    dlsetresolution(1);

  } else if(streq(argv[1], "prio")){
    if(argc < 3){ printf("dlmode prio needs a value 0-9\n"); exit(1); }
    int p = atoi(argv[2]);
    if(setpriority(p) < 0)
      printf("Invalid priority (use 0-9)\n");
    else
      printf("Priority set to %d\n", p);

  } else if(streq(argv[1], "status")){
    dlstate();

  } else {
    usage();
    exit(1);
  }

  exit(0);
}
