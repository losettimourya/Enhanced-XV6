#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char **argv)
{
    if (argc - 3)
    {
        fprintf(2, "Wrong no of arguments\n");
        exit(1);
    }
    int priority = atoi(argv[1]);
    if (priority >= 0 && priority <= 100)
    {
        int pid = atoi(argv[2]);
        set_priority(priority, pid);
        exit(1);
    }
    else
    {
        fprintf(2, "Priority isn't within the limit\n");
        exit(1);
    }
}
