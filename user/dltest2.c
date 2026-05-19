#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// dltest2: three process circular deadlock with real file access
//
// the tokens here act as mutexes (locks) protecting real os files
// this is how real applications use mutex locks in practice
//
// process A: holds token0 (file0), wants token1 (file1)
// process B: holds token1 (file1), wants token2 (file2)
// process C: holds token2 (file2), wants token0 (file0)
//
// the dfs algorithm must follow the chain A -> B -> C -> A to find the cycle
// this proves the detection works for more than just two processes

// write something to a file to prove we actually used it
static void write_to_file(int fd, int proc_id)
{
    char buf[4];
    buf[0] = 'p';
    buf[1] = '0' + proc_id;
    buf[2] = '\n';
    buf[3] = '\0';
    write(fd, buf, 3);
}

int main(void)
{
    int pid_b;
    int pid_c;
    int fd;
    int ret;

    printf("=== dltest2: three process circular deadlock ===\n");
    printf("A -> B -> C -> A cycle\n");
    printf("tokens protect real file access (like mutexes)\n\n");

    // create three real files
    fd = open("testfile0", O_RDWR | O_CREATE);
    if(fd >= 0) close(fd);
    fd = open("testfile1", O_RDWR | O_CREATE);
    if(fd >= 0) close(fd);
    fd = open("testfile2", O_RDWR | O_CREATE);
    if(fd >= 0) close(fd);

    pid_b = fork();
    if(pid_b < 0)
    {
        printf("fork failed\n");
        exit(1);
    }

    if(pid_b == 0)
    {
        // we are in process B's child, fork again to make C
        pid_c = fork();
        if(pid_c < 0)
        {
            printf("fork failed\n");
            exit(1);
        }

        if(pid_c == 0)
        {
            // process C: token2 first then token0
            printf("process C (pid=%d): getting token 2...\n", getpid());
            ret = dlacquire(2);
            if(ret < 0)
            {
                printf("process C: failed getting token 2\n");
                exit(1);
            }

            // do some real file work
            fd = open("testfile2", O_RDWR);
            if(fd >= 0)
            {
                write_to_file(fd, getpid());
                close(fd);
            }

            printf("process C (pid=%d): got token 2, waiting...\n", getpid());
            pause(5);

            printf("process C (pid=%d): trying token 0 (deadlock here)\n", getpid());
            ret = dlacquire(0);
            if(ret < 0)
            {
                printf("process C (pid=%d): was picked as victim (code %d)\n", getpid(), ret);
                dlrelease(2);
                exit(1);
            }

            printf("process C (pid=%d): got token 0, releasing\n", getpid());
            dlrelease(0);
            dlrelease(2);
            exit(0);
        }
        else
        {
            // process B: token1 first then token2
            printf("process B (pid=%d): getting token 1...\n", getpid());
            ret = dlacquire(1);
            if(ret < 0)
            {
                printf("process B: failed getting token 1\n");
                wait(0);
                exit(1);
            }

            fd = open("testfile1", O_RDWR);
            if(fd >= 0)
            {
                write_to_file(fd, getpid());
                close(fd);
            }

            printf("process B (pid=%d): got token 1, waiting...\n", getpid());
            pause(5);

            printf("process B (pid=%d): trying token 2 (deadlock here)\n", getpid());
            ret = dlacquire(2);
            if(ret < 0)
            {
                printf("process B (pid=%d): was picked as victim (code %d)\n", getpid(), ret);
                dlrelease(1);
                wait(0);
                exit(1);
            }

            printf("process B (pid=%d): got token 2, releasing\n", getpid());
            dlrelease(2);
            dlrelease(1);
            wait(0);
            exit(0);
        }
    }
    else
    {
        // process A: token0 first then token1
        printf("process A (pid=%d): getting token 0...\n", getpid());
        ret = dlacquire(0);
        if(ret < 0)
        {
            printf("process A: failed getting token 0\n");
            wait(0);
            exit(1);
        }

        fd = open("testfile0", O_RDWR);
        if(fd >= 0)
        {
            write_to_file(fd, getpid());
            close(fd);
        }

        printf("process A (pid=%d): got token 0, waiting...\n", getpid());
        pause(5);

        printf("process A (pid=%d): trying token 1 (deadlock here)\n", getpid());
        ret = dlacquire(1);
        if(ret < 0)
        {
            printf("process A (pid=%d): was picked as victim (code %d)\n", getpid(), ret);
            dlrelease(0);
            wait(0);
            exit(1);
        }

        printf("process A (pid=%d): got token 1, releasing\n", getpid());
        dlrelease(1);
        dlrelease(0);
        wait(0);
        printf("=== dltest2 done ===\n");
    }

    exit(0);
}
