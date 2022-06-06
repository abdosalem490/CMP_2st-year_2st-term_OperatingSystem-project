#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>


typedef short boolean;
#define true 1
#define false 0

#define SHKEY 400

#define EOF_ARRIVAL_FIFO "/tmp/end_of_arrival_fifo"
#define PROCESS_ARRIVAL_FIFO "/tmp/process_arrival_fifo"
#define PROCESS_FIFO "/tmp/process_fifo"
#define ACK_FIFO "/tmp/ack_fifo"
#define SHM_FIFO "/tmp/process_shm_fifo"

#define MAX_PID_LENGTH 12

// process contains the basic input-supplied values about the processes.
struct process {
    int id;
    int arrival;
    int runtime;
    int priority;
    int mem;
};

/*
    Used in process_generator.c

    arrivalProcess is the node struct of the doubly-linked list containing the processes
    that have the same arrival time.
*/
struct arrivalProcess {
    struct pNode *pNode;
    struct arrivalProcess *next;
};

/*
    Used in process_generator.c
*/
struct pNode {
    struct process *process;
    struct pNode *next;
};

/*
    Further illustration:
    Let a, b, c be three arrivalProcess, such that:
    
    a has an arrival time of 5
    b has an arrival time of 7
    c has an arrival time of 15

    -> : indicates next of the left node is the right node. (arrivalProcess)
    | : indicates next of the above node is the one below it. (pNodes)

    nodes a->pNode, a1, a2, a3 all have arrival time of 5
    nodes b->pNode, b1, b2, b3 all have arrival time of 7
    nodes c->pNode, c1, c2, c3 all have arrival time of 15

    a -> b -> c
    |    |    |
    a1   b1   c1
    |    |    |
    a2   b2   c2
    |    |    |
    a3   b3   c3

*/


/* 
    Used in scheduler.c
    queuedProcess is the node struct of the doubly-linked list containing the processes queued.
*/
struct queuedProcess {
    struct pcb *pcb;
    struct queuedProcess *next;
    struct queuedProcess *previous;
};

// pcb is the process control block.
struct pcb {
    
    struct process *process;
    
    pid_t pid;
    
    // Finish time
    int ft;
    
    // Start time
    int st;
    
    // Remaining time
    int rt;

    // Waiting time
    int wt;

    // Last stopped time: last time the process was stopped.
    int lst;

    // Last resuming time: last time the process was resumed.
    int lrt;

    /* 
        Queuing criteria
        In the case of HPF, this refers to the pcb->p->priority.
        In the case of SRTN, this refers to the pcb->rt;

        This is done to generalize the queueing process in the function insertQueued in the scheduler.c file.
        It always inserts sorted ascendingly by *q.
    */
    int *q;

    // State
    /*
        'R' = RUNNING
        'T' = STOPPED
        'Q' = QUEUED (NOT YET RUN = NEEDS FORKING).
    */
    char state;

    // The nearest power of two of the memory size.
    int nearestPower;

    // The memory node in the tree
    struct memoryNode *memoryNode;

    key_t shmKey;

    int shmid;
};

// A macro to create a new pcb struct an initialize its values to the default values.
#define INIT_PCB (struct pcb){NULL, -1, -1, -1, -1, 0, -1, -1, NULL, 'Q', -1, NULL, -1, -1}


/*****************************************************************************************************************************************************/

/*
    Memory-related..
*/

/*
    memoryNode is the struct for the nodes in the memory binary tree.
    members:
        right:      the right child.
        left:       the left child.
        parent:     the parent (for ease of traversal).
        start:      the start byte of the node. 
                    this is equal to parent->start if it's the right child, and to parent->start + capacity if it's the left child.
        capacity:   the capacity of the node (always a power of two).
*/
struct memoryNode {
    struct memoryNode *right;
    struct memoryNode *left;
    struct memoryNode *parent;
    int start;
    int capacity;
    boolean full;
};

// A macro to initialize a memoryNode
#define INIT_MEM_NODE (struct memoryNode){NULL, NULL, NULL, -1, 0, false}

/*****************************************************************************************************************************************************/

int getClk();

/*
 * All process call this function at the beginning to establish communication between them and the clock module.
 * Again, remember that the clock is only emulation!
*/
void initClk();

void destroyClk();
