#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"
#include <stddef.h>

int main(int argc, char *argv[])
{
    int num_arguments = argc;
    int first_digit = argv[1][0];
    int flag = 0;
    if ((first_digit > 47) && (first_digit < 58))
    {
        flag = 1;
    }
    if ((num_arguments >= 3) && (num_arguments <= 32) && flag)
    {
        int mask_argument = atoi(argv[1]);
        char *copy_arguments[32];
        int return_value = strace(mask_argument);
        if (return_value < 0)
        {
            printf("strace command has failed\n");
            exit(1);
        }
        for (int i = 0; i < num_arguments - 2; i++)
        {
            copy_arguments[i] = argv[i + 2];
        }
        exec(copy_arguments[0], copy_arguments);
        exit(1);
    }
    else
    {
        printf("Please enter commands in the following format:\n");
        printf("strace mask command [args]\n");
        exit(1);
    }
}