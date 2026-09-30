#include <stdio.h>

int main(int argc, char **argv)
{
    const char *appName = (argc > 1) ? argv[1] : "Application";
    printf("\n======================================================\n");
    printf(" [CELS HOT-RELOAD] %s dynamic library rebuilt successfully!\n", appName);
    printf(" The running host will detect the timestamp and reload.\n");
    printf("======================================================\n\n");
    return 0;
}
