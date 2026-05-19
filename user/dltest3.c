#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dltest3: shows that the scoring system picks the right victim
//
// we create a deadlock between two processes
// one process gets priority 9 (very important, should survive)
// the other process gets priority 0 (not important, should be killed)
//
// run this test with: dltest3
// the low priority process should always be killed, not the high priority one

int main(void)
{
    int child_pid;
    int ret;

    printf("=== dltest3: priority based victim selection ===\n");
    printf("high priority process (prio=9) should always survive\n");
    printf("low priority process (prio=0) should always be killed\n\n");

    child_pid = fork();
    if(child_pid < 0)
    {
        printf("fork failed\n");
        exit(1);
    }

    if(child_pid == 0)
    {
        // child process gets LOW priority
        setpriority(0);

        printf("LOW PRIORITY process (pid=%d prio=0): getting token 3...\n", getpid());
        ret = dlacquire(3);
        if(ret < 0)
        {
            printf("LOW: could not get token 3\n");
            exit(1);
        }

        printf("LOW PRIORITY process (pid=%d): got token 3, waiting...\n", getpid());
        pause(5);

        printf("LOW PRIORITY process (pid=%d): trying token 4 (deadlock here)\n", getpid());
        ret = dlacquire(4);

        if(ret < 0)
        {
            // this is what we expect
            printf("LOW PRIORITY process (pid=%d): killed as expected, system works correctly\n", getpid());
            dlrelease(3);
            exit(1);
        }

        // if we reach here something went wrong
        printf("LOW PRIORITY process: WARNING should have been killed but wasnt\n");
        dlrelease(4);
        dlrelease(3);
        exit(0);
    }
    else
    {
        // parent process gets HIGH priority
        setpriority(9);

        printf("HIGH PRIORITY process (pid=%d prio=9): getting token 4...\n", getpid());
        ret = dlacquire(4);
        if(ret < 0)
        {
            printf("HIGH: could not get token 4\n");
            wait(0);
            exit(1);
        }

        printf("HIGH PRIORITY process (pid=%d): got token 4, waiting...\n", getpid());
        pause(5);

        printf("HIGH PRIORITY process (pid=%d): trying token 3 (deadlock here)\n", getpid());
        ret = dlacquire(3);

        if(ret < 0)
        {
            // this should NOT happen since high priority should be protected
            printf("HIGH PRIORITY process (pid=%d): was killed, scoring has a bug!\n", getpid());
            dlrelease(4);
            wait(0);
            exit(1);
        }

        printf("HIGH PRIORITY process (pid=%d): survived as expected, priority scoring is correct\n", getpid());
        dlrelease(3);
        dlrelease(4);
        wait(0);
        printf("=== dltest3 done ===\n");
    }

    exit(0);
}
