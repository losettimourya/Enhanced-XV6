#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    int ntest = 10;
    int child = 0;
    for (int i = 0; i < ntest; i++)
    {
        int pid = fork();
        if (pid < 0)
        {
            printf("fork failed\n");
        }
        else if (pid == 0)
        {
            int curr_child = child;
            settickets(ntest - curr_child);
            for (long long i = 0; i < 5000000000; i++)
            {
            }
            printf("Exiting from child %d\n",curr_child);
            exit(0);
        }
        else
        {
            child++;
        }
    }
    return 0;
}