#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dltest4: shows preemption mode where no process gets killed
//
// same circular deadlock as dltest but using preemption instead of kill
// the victim process gets its resources stripped and receives error -3
// it can then exit gracefully instead of being terminated
//
// before running this do: dlmode preempt
// after running do: dlmode kill  (to go back to normal)
//
// expected result: both processes print a message and exit cleanly
// no process should be killed

int main(void)
{
    int child_pid;
    int ret;

    printf("=== dltest4: preemption mode demo ===\n");
    printf("no process will be killed in this test\n");
    printf("victim loses resources but continues running\n");
    printf("make sure you ran: dlmode preempt\n\n");

    child_pid = fork();
    if(child_pid < 0)
    {
        printf("fork failed\n");
        exit(1);
    }

    if(child_pid == 0)
    {
        // process B
        printf("process B (pid=%d): getting token 5...\n", getpid());
        ret = dlacquire(5);
        if(ret < 0)
        {
            printf("process B: failed to get token 5\n");
            exit(1);
        }

        printf("process B (pid=%d): got token 5, waiting...\n", getpid());
        pause(5);

        printf("process B (pid=%d): trying token 6 (deadlock here)\n", getpid());
        ret = dlacquire(6);

        if(ret == -3)
        {
            // this is the preempt case, our resources were stripped
            printf("process B (pid=%d): preempted, resources stripped, exiting cleanly\n", getpid());
            // kernel already freed our resources so no need to release
            exit(0);
        }
        else if(ret == -2)
        {
            // kill mode was probably still on
            printf("process B (pid=%d): killed (did you set dlmode preempt?)\n", getpid());
            exit(1);
        }

        printf("process B (pid=%d): got token 6, releasing\n", getpid());
        dlrelease(6);
        dlrelease(5);
        exit(0);
    }
    else
    {
        // process A
        printf("process A (pid=%d): getting token 6...\n", getpid());
        ret = dlacquire(6);
        if(ret < 0)
        {
            printf("process A: failed to get token 6\n");
            wait(0);
            exit(1);
        }

        printf("process A (pid=%d): got token 6, waiting...\n", getpid());
        pause(5);

        printf("process A (pid=%d): trying token 5 (deadlock here)\n", getpid());
        ret = dlacquire(5);

        if(ret == -3)
        {
            printf("process A (pid=%d): preempted, resources stripped, exiting cleanly\n", getpid());
            wait(0);
            exit(0);
        }
        else if(ret == -2)
        {
            printf("process A (pid=%d): killed (did you set dlmode preempt?)\n", getpid());
            dlrelease(6);
            wait(0);
            exit(1);
        }

        printf("process A (pid=%d): got token 5, releasing\n", getpid());
        dlrelease(5);
        dlrelease(6);
        wait(0);
        printf("=== dltest4 done ===\n");
    }

    exit(0);
}
