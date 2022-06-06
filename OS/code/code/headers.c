#include "headers.h"


///==============================
//don't mess with this variable//
int * shmaddr;                 //
//===============================

void destroyClk()
{
    shmdt(shmaddr);
}


int getClk()
{
    return *shmaddr;
}

void initClk()
{
    int shmid = shmget(SHKEY, 4, 0444);
    while ((int)shmid == -1)
    {
        //Make sure that the clock exists
        printf("Wait! The clock not initialized yet!\n");
        sleep(1);
        shmid = shmget(SHKEY, 4, 0444);
    }
    shmaddr = (int *) shmat(shmid, (void *)0, 0);
}