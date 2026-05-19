#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dltest: basic two process deadlock
// process A grabs token 0 then wants token 1
// process B grabs token 1 then wants token 0
// this creates a circular wait and the system should detect and fix it

int main(void)
{
    int child_pid;
    int ret;

    printf("=== dltest: basic two process deadlock ===\n");
    printf("process A holds token0 and wants token1\n");
    printf("process B holds token1 and wants token0\n\n");

    child_pid = fork();

    if(child_pid < 0)
    {
        printf("fork failed\n");
        exit(1);
    }

    if(child_pid == 0)
    {
        // this is process B (child)
        printf("process B (pid=%d): trying to get token 1...\n", getpid());

        ret = dlacquire(1);
        if(ret < 0)
        {
            printf("process B: could not get token 1\n");
            exit(1);
        }

        printf("process B (pid=%d): got token 1, waiting a bit...\n", getpid());
        pause(5);

        printf("process B (pid=%d): now trying token 0 (deadlock will happen here)\n", getpid());

        ret = dlacquire(0);
        if(ret == -2)
        {
            printf("process B (pid=%d): was killed by the resolver\n", getpid());
            dlrelease(1);
            exit(1);
        }
        else if(ret == -3)
        {
            printf("process B (pid=%d): resources were taken (preempt mode)\n", getpid());
            dlrelease(1);
            exit(0);
        }

        printf("process B (pid=%d): got token 0, releasing everything\n", getpid());
        dlrelease(0);
        dlrelease(1);
        exit(0);
    }
    else
    {
        // this is process A (parent)
        printf("process A (pid=%d): trying to get token 0...\n", getpid());

        ret = dlacquire(0);
        if(ret < 0)
        {
            printf("process A: could not get token 0\n");
            wait(0);
            exit(1);
        }

        printf("process A (pid=%d): got token 0, waiting a bit...\n", getpid());
        pause(5);

        printf("process A (pid=%d): now trying token 1 (deadlock will happen here)\n", getpid());

        ret = dlacquire(1);
        if(ret == -2)
        {
            printf("process A (pid=%d): was killed by the resolver\n", getpid());
            dlrelease(0);
            wait(0);
            exit(1);
        }
        else if(ret == -3)
        {
            printf("process A (pid=%d): resources were taken (preempt mode)\n", getpid());
            dlrelease(0);
            wait(0);
            exit(0);
        }

        printf("process A (pid=%d): got token 1, releasing everything\n", getpid());
        dlrelease(1);
        dlrelease(0);
        wait(0);
        printf("=== dltest done ===\n");
    }

    exit(0);
}
