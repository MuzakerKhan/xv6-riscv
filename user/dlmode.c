#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dlmode: command to control the deadlock system settings
//
// usage:
//   dlmode normal        switch to normal mode (less overhead)
//   dlmode aggressive    switch to aggressive mode (checks everything)
//   dlmode kill          set resolution to kill the victim
//   dlmode preempt       set resolution to strip resources, keep process alive
//   dlmode prio <0-9>    set this process priority for scoring
//   dlmode status        print current system state

// simple string comparison since we cant use strcmp in some environments
static int str_equal(const char *a, const char *b)
{
    while(*a && *b && *a == *b)
    {
        a++;
        b++;
    }
    return (*a == 0 && *b == 0);
}

static void print_usage(void)
{
    printf("usage:\n");
    printf("  dlmode normal        - low overhead recording mode\n");
    printf("  dlmode aggressive    - full deadlock checking on every access\n");
    printf("  dlmode kill          - kill the victim when deadlock found\n");
    printf("  dlmode preempt       - strip victim resources, keep process alive\n");
    printf("  dlmode prio <0-9>    - set this process priority (0=low, 9=high)\n");
    printf("  dlmode status        - show current deadlock system state\n");
}

int main(int argc, char *argv[])
{
    int prio_val;

    if(argc < 2)
    {
        print_usage();
        exit(1);
    }

    if(str_equal(argv[1], "normal"))
    {
        dlsetmode(0);
    }
    else if(str_equal(argv[1], "aggressive"))
    {
        dlsetmode(1);
    }
    else if(str_equal(argv[1], "kill"))
    {
        dlsetresolution(0);
    }
    else if(str_equal(argv[1], "preempt"))
    {
        dlsetresolution(1);
    }
    else if(str_equal(argv[1], "prio"))
    {
        if(argc < 3)
        {
            printf("dlmode prio needs a number from 0 to 9\n");
            exit(1);
        }
        prio_val = atoi(argv[2]);
        if(setpriority(prio_val) < 0)
            printf("invalid priority, use 0 to 9\n");
        else
            printf("priority set to %d\n", prio_val);
    }
    else if(str_equal(argv[1], "status"))
    {
        dlstate();
    }
    else
    {
        printf("unknown option: %s\n", argv[1]);
        print_usage();
        exit(1);
    }

    exit(0);
}
