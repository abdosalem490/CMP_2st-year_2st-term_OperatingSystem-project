#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#define _GNU_SOURCE
#include "headers.h"

int eoArrivalFifo, processArrivalFifo, ackFifo;
int clk, algo, quantum = -1;
pid_t schedulerPid, clkPid;
FILE *input;
char **gargv;

#define likely(x)  __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)


static void clearResources(int);

int main(int argc, char * argv[])
{

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    signal(SIGINT, clearResources);
    gargv = argv;

    // 1. Read the input files.
    if (argc < 3) {
        fprintf(stderr, "usage: %s [input-file] [scheduling-algorithm]\n", argv[0]);
        // fprintf(stderr, )
        exit(EXIT_FAILURE);
    }
    
    algo = atoi(argv[2]);
    if (algo > 2) {
        fprintf(stderr, "values allowed for [scheduled-algorithm] are: 0 [HPF], 1 [SRTN], 2 [RR]\n");
        exit(EXIT_FAILURE); 
    } 
    if (algo == 2) {
        if (argc < 4) {
            fprintf(stderr, "supply the quantum in seconds for the RR algorithm as a fourth parameter.\n");
            exit(EXIT_FAILURE);
        } else
            quantum = atoi(argv[3]);
    }

    // Opening the supplied input file.
    input = fopen(argv[1], "r");
    if (input == NULL) {
        raise(SIGINT);
    }

    ssize_t r; size_t len = 0; char *line = NULL;

    // Creating and filling the head node with data.
    struct arrivalProcess *head = malloc(sizeof(struct arrivalProcess));
    struct arrivalProcess *currArrivalProcess = head;
    struct pNode *currPNode;


    while ((r = getline(&line, &len, input)) != -1) {
        if (line[0] == '#')
            continue;
        else {
            head->pNode = malloc(sizeof(struct pNode));
            head->pNode->process = malloc(sizeof(struct process));
            head->pNode->process->id = atoi(strtok(line, "\t"));
            head->pNode->process->arrival = atoi(strtok(NULL, "\t"));
            head->pNode->process->runtime = atoi(strtok(NULL, "\t"));
            head->pNode->process->priority = atoi(strtok(NULL, "\t"));
            head->pNode->process->mem = atoi(strtok(NULL, "\t"));
            head->pNode->next = NULL;

            head->next = NULL;
            currPNode = head->pNode;
            break;
        }
    }

    // Creating the other nodes..
    struct process *p;

    while ((r = getline(&line, &len, input)) != -1) {
        
        if (line[0] == '#')
            continue;
        
        p = malloc(sizeof(struct process));
        p->id = atoi(strtok(line, "\t"));
        p->arrival = atoi(strtok(NULL, "\t"));
        p->runtime = atoi(strtok(NULL, "\t"));
        p->priority = atoi(strtok(NULL, "\t"));
        p->mem = atoi(strtok(NULL, "\t"));
        
        // If the arrival time of this process is equal to the current arrivalProcess:
        if (p->arrival == currArrivalProcess->pNode->process->arrival) {
            currPNode->next = malloc(sizeof(struct pNode));
            currPNode->next->process = p;
            currPNode = currPNode->next;
        } else {

            // Create a new arrivalProcessNode.
            currArrivalProcess->next = malloc(sizeof(struct arrivalProcess));
            currArrivalProcess = currArrivalProcess->next;
            currArrivalProcess->next = NULL;

            // Create the first pNode inside it.
            currArrivalProcess->pNode = malloc(sizeof(struct pNode));
            currArrivalProcess->pNode->process = p;
            currArrivalProcess->pNode->next = NULL;

            currPNode = currArrivalProcess->pNode;
        }
    }

    fclose(input);
    input = NULL;

    if (mkfifo(EOF_ARRIVAL_FIFO, S_IRUSR | S_IWUSR | S_IRGRP) == -1 && errno != EEXIST) {
        fprintf(stderr, "=> %s: error creating the EOF_ARRIVAL_FIFO", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (mkfifo(PROCESS_ARRIVAL_FIFO, S_IRUSR | S_IWUSR | S_IRGRP) == -1 && errno != EEXIST) {
        fprintf(stderr, "=> %s: error creating the PROCESS_ARRIVAL_FIFO", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (mkfifo(ACK_FIFO, S_IRUSR | S_IWUSR | S_IRGRP) == -1 && errno != EEXIST) {
        fprintf(stderr, "=> %s: error creating the ACK_FIFO", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Creating the Scheduler process.
    schedulerPid = fork();
    switch(schedulerPid) {
        case 0: {

            char *a = malloc(2 * sizeof(char));
            sprintf(a, "%d", algo);
            char *qb;
            if (algo == 2)
                qb = strdup(argv[3]);
            else
                qb = "-1";

            fprintf(stderr, "=> ./scheduler: executing the scheduler: %ld\n", (long) getpid());
            execlp("./scheduler", "./scheduler", a, qb, (char *)NULL);
            fprintf(stderr, "=> %s: failed execlp: %m. errno: %d.", argv[0], errno);
            exit(EXIT_FAILURE);
        }
        case -1:
            raise(SIGINT);
        default:
            break;
    }

    int more = 1, none = 0, ack = 0;
    currArrivalProcess = head;

    // Open the FIFOs for writing. This will block until the scheduler opens it for reading..
    eoArrivalFifo = open(EOF_ARRIVAL_FIFO, O_WRONLY);
    processArrivalFifo = open(PROCESS_ARRIVAL_FIFO, O_WRONLY);
    ackFifo = open(ACK_FIFO, O_RDONLY);


    if (processArrivalFifo == -1 || eoArrivalFifo == -1 || ackFifo == -1) {
        fprintf(stderr, "=> %s: error creating the process FIFO or the arrival FIFO: %m. errno: %d", argv[0], errno);
        raise(SIGINT);
    }

    int selectReturn;
    struct timeval timeout;
    timeout.tv_sec = 0; timeout.tv_usec = 50000;
    
    // Creating the clock process first.
    clkPid = fork();
    switch (clkPid) {
        case 0: {
            
            close(processArrivalFifo);
            close(eoArrivalFifo);
            close(ackFifo);

            execlp("./clk", "./clk", (char *)NULL);
            exit(EXIT_FAILURE);
        }
        case -1:
            raise(SIGINT);
            break;
        default:
            break;
    }

    initClk();

    while(currArrivalProcess) {

        clk = getClk();
        if (clk >= currArrivalProcess->pNode->process->arrival) {

            for (struct pNode *pNode = currArrivalProcess->pNode; pNode; pNode = pNode->next) {
                

                // Here, writing the process to the processArrivalFifo.
                len = write(processArrivalFifo, pNode->process, sizeof(struct process));
                if (len == -1 || len < sizeof(struct process))
                    fprintf(stderr, "=> %s: error writing to the processArrivalFifo: %m. errno: %d\n", argv[0], errno);

                if (read(ackFifo, &ack, sizeof(int)) != sizeof(int) || ack != pNode->process->id)
                    fprintf(stderr, "=> %s: error reading: %m. errno: %d. ack: %d, expecting: %d\n", argv[0], errno , ack, pNode->process->id);

                switch (len) {
                    case -1:
                        fprintf(stderr, "=> %s: error happened writing to the process FIFO: %m. errno: %d.\n", argv[0], errno);
                    default:
                        if (len < sizeof(struct process))
                            fprintf(stderr, "=> %s: partial write error while writing to the process FIFO.\n", argv[0]);
                        break;
                }
    
                if (pNode->next) {
                    write(eoArrivalFifo, &more, sizeof(int));
                } else {
                    write(eoArrivalFifo, &none, sizeof(int));
                    break;
                }
            }
            currArrivalProcess = currArrivalProcess->next;
        } else {

            selectReturn = select(0, NULL, NULL, NULL, &timeout);
            if (unlikely(selectReturn == -1))
                fprintf(stderr, "=> %s: error selecting/sleeping: %m. errno: %d\n", argv[0], errno);
            timeout.tv_usec = 10000; // 10 ms
        
        }  
    }

    fprintf(stderr, "=> %s: finished sending all the processes.\n", argv[0]);
    
    clearResources(-1);    
}


/*
    A signal handler for SIGINT.
    It's also called when reaching the end of main normally, with an argument signum = -1.
*/
static void 
clearResources(int signum)
{
    close(processArrivalFifo);        
    close(eoArrivalFifo);
    close(ackFifo);

    if (input)
        fclose(input);

    // Detaching the shared memory pointer. 
    destroyClk();

    // Waiting on the scheduler process.
    if (waitpid(schedulerPid, NULL, 0) == -1)
        fprintf(stderr, "error waiting on scheduler process: %m. errno: %d\n", errno);
    
    // If we've normally terminated, and the scheduler has returned, kill the clock.
    // If we have not, the SIGINT sent through the terminal already terminated the clock.
    if (signum == -1)
        kill(clkPid, SIGINT);
    
    // Wait on the clock process.
    if (waitpid(clkPid, NULL, 0) == -1)
        fprintf(stderr, "error waiting on clock process: %m. errno: %d\n", errno);

    exit(EXIT_SUCCESS);
}
