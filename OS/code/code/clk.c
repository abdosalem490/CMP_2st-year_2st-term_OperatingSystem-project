/*
 * This file is done for you.
 * Probably you will not need to change anything.
 * This file represents an emulated clock for simulation purpose only.
 * It is not a real part of operating system!
 */
#define _GNU_SOURCE
#include "headers.h"

int shmid;


/* Clear the resources before exit */
static void 
cleanup(int signum)
{
    shmctl(shmid, IPC_RMID, NULL);
    printf("Clock terminating!\n");
    exit(0);
}

/* This file represents the system clock for ease of calculations */
int main(int argc, char * argv[])
{
    fprintf(stderr, "=> %s: Clock starting\n", argv[0]);
    signal(SIGINT, cleanup);

    int clk = 0;
    //Create shared memory for one integer variable 4 bytes
    shmid = shmget(SHKEY, 4, IPC_CREAT | 0644);
    if ((long)shmid == -1)
    {
        perror("Error in creating shm!");
        exit(-1);
    }
    int * shmaddr = (int *) shmat(shmid, (void *)0, 0);
    if ((long)shmaddr == -1)
    {
        perror("Error in attaching the shm in clock!");
        exit(-1);
    }
    *shmaddr = clk; /* initialize shared memory */

    int selectReturn;
    struct timeval timeout;
    timeout.tv_sec = 1; timeout.tv_usec = 0;

    while (true) {
        selectReturn = select(0, NULL, NULL, NULL, &timeout);
        if (selectReturn == -1)
            perror("Error selecting/sleeping");
        
        (*shmaddr)++;
        timeout.tv_sec = 1; timeout.tv_usec = 0;
    }
}