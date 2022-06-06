#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define null 0

struct processData
{
    int arrivaltime;
    int priority;
    int runningtime;
    int id;
    int memsize;
};

int main(int argc, char * argv[])
{
    FILE * pFile;
    pFile = fopen("processes.txt", "w");
    int no;
    struct processData pData;
    if (argc < 2) {
        printf("Please supply the number of processes you want to generate as an argument.\n");
        exit(EXIT_FAILURE);
    }
    no = atoi(argv[1]);

    srand(time(null));
    //fprintf(pFile,"%d\n",no);
    fprintf(pFile, "#id arrival runtime priority memsize\n");
    pData.arrivaltime = 1;
    for (int i = 1 ; i <= no ; i++)
    {
        //generate Data Randomly
        //[min-max] = rand() % (max_number + 1 - minimum_number) + minimum_number
        pData.id = i;
        pData.arrivaltime += rand() % (11); //processes arrives in order
        pData.runningtime = rand() % (30);
        pData.priority = rand() % (11);
        pData.memsize = rand() % 200;
        fprintf(pFile, "%d\t%d\t%d\t%d\t%d\n", pData.id, pData.arrivaltime, pData.runningtime, pData.priority, pData.memsize);
    }
    fclose(pFile);
}
