#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// dlmon: deadlock monitoring tool
// prints the current system state every second
// runs 10 times then stops automatically
// run it while another dltest is running to watch the deadlock happen

int main(void)
{
    int round;
    int total_rounds;

    total_rounds = 10;

    for(round = 1; round <= total_rounds; round++)
    {
        printf("\n--- Deadlock Monitor (round %d of %d) ---\n", round, total_rounds);
        dlstate();
        pause(10);
    }

    printf("dlmon: finished monitoring\n");
    exit(0);
}
