#include <stdio.h>
#define _GNU_SOURCE
#include "headers.h"
#include <math.h>
#include <time.h>

// Structs
struct queuedProcess *readyHead = NULL, *readyLast = NULL, *currentlyRunning = NULL;
struct queuedProcess *waitingHead = NULL, *waitingLast = NULL;

// Metrics
float ta, wta, totalWTASquared = 0;
int cpuIdleTime = 0, lastIdle = 0, totalRuntime = 0, numOfProcess = 0, totalWTA = 0, totalWaiting = 0;

// Fifos
int eoArrivalFifo, processArrivalFifo, shmFifo, processFifo, ackFifo;
int isProcessArrivalFifoEOF = 0, isProcessFifoEOF = 0;

// Algo-related
int clk, moreOrNone, nfds, selectReturn, algo, signalAck, qunatum;
char *pidOfFinished, **gargv, *shmKeyFileGen = "/tmp/scheduler-key-process";
boolean hasHadItsQuantum = false;

// Memory-related
struct memoryNode *memRoot = NULL;
struct memoryNode *latestMemAllocation;
int totalAvailMem = 1024;

// Misc.
fd_set readFds;
ssize_t r;
pid_t pid;
FILE *logger, *metrics, *memoryLogger;

/*
    General.
*/

/*
    This function finds the nearest power of two to the given integer.
*/
static int
nearestPowerOfTwo(int toFindFor) 
{
    int powerOfTwo = 1;
    while (toFindFor > powerOfTwo)
        powerOfTwo *= 2;

    return powerOfTwo;
}

/*
    This function implements the buddy memory allocation system.
    This is a recursive function, and is wrapped with the allocate() function below.
    
    params:
        root:           the current node.
        parent:         the parent of the current node.
        leftOrRight:    needed to indicate whether this node is the left or the right child of the parent.
                        note: that you may think this could be avoided with checking if the root = parent->right or root == parent->left,
                              but it's necessary because, the way I implemented the function, sometimes root is equal to NULL, and so we cannot
                              compare this NULL.
        nearestPower:   the nearest power of 2 to the size.
        size:           the actual memory size that will be used.  

    On how I have implemented the buddy algorithm, check: 
        https://www.youtube.com/watch?v=1pCC6pPAtio
                           
*/
static struct memoryNode*
buddyAllocater(struct memoryNode *root, struct memoryNode *parent, int leftOrRight, int nearestPower, int size) 
{

    // Create it, and check the capacity.
    if (root == NULL) {

        // right = 1, left = 0
        if (leftOrRight == 1) {
            parent->right = malloc(sizeof(struct memoryNode));
            *(parent->right) = INIT_MEM_NODE;
            root = parent->right;
            root->parent = parent;
            root->capacity = parent->capacity / 2;
            root->start = parent->start;
        } else if (leftOrRight == 0) {
            parent->left = malloc(sizeof(struct memoryNode));
            *(parent->left) = INIT_MEM_NODE;
            root = parent->left;
            root->parent = parent;
            root->capacity = (parent->capacity) / 2;
            root->start = parent->start + root->capacity;
        } else {
            memRoot = malloc(sizeof(struct memoryNode));
            *memRoot = INIT_MEM_NODE;
            memRoot->capacity = 1024;
            memRoot->start = 0;
            root = memRoot;
        }

        // Allocate if equal
        if (root->capacity == nearestPower) {
            root->full = true;
            return root;
        } else {
            // If greater..
            struct memoryNode* m = buddyAllocater(root->right, root, 1, nearestPower, size);
            if (m)
                return m;

            m = buddyAllocater(root->left, root, 0, nearestPower, size);
            return m;
        }
        
    } 
    
    // As long as it exists and its capacity is what we desire, it has actually less than the desired available..
    // Whether it's a leaf, or there are further children under it.
    else if (root->capacity == nearestPower) {
        return NULL;
    } 
    
    // If we reached a leaf, no matter what its capacity is, backtrack to the parent.
    else if (root->full == true) {
        return NULL;
    } 
    
    // If not all the above, and the capacity is greater than t he desired nearest power, descend further.
    else if (root->capacity > nearestPower) {
        
        struct memoryNode* m = buddyAllocater(root->right, root, 1, nearestPower, size);
        if (m)
            return m;

        m = buddyAllocater(root->left, root, 0, nearestPower, size);
        return m;
        
    }

    return NULL;

}

/*
    This function is a wrapper for the buddyAlgorithm function,
    reducing boilerplate code, in that it'll always be called first through root.

    params:
        size: the memory size to be allocated.
        nearestPower: the nearest power of 2 to the requested size.
    
    returns:
        the return value of buddy's function.
*/
static struct memoryNode*
buddyAllocatorWrapper(int size, int nearestPower)
{
    // Pruning cases where the total available memory for starters is anyways less than the required.
    // No need to traverse the tree then!
    if (nearestPower > totalAvailMem)
        return NULL;

    return buddyAllocater(memRoot, NULL, -1, nearestPower, size);
}

static key_t
generateShmKey(char *filePath)
{
    // Generate a random SHKEY.    
    int file = open(filePath, O_CREAT | O_RDONLY | O_EXCL, S_IRUSR | S_IWUSR);
    if (file == -1) {
        free(filePath);
        filePath = shmKeyFileGen;
    }

    srand(time(0) + getpid());
    key_t key = ftok(filePath, rand() % 10000);

    for (int i = 0; i < 3 && key == -1; i++) {
        fprintf(stderr, "=> %s: error creating a new key: %m. errno: %d\n", gargv[0], errno);
        key = ftok(filePath, rand() % 10000);
    }

    // Deleting the key file (no longer needed) if it's not the fallback general file "/tmp/scheduler-key-process"
    if (filePath != shmKeyFileGen) {
        unlink(filePath);
        free(filePath);
    }

    filePath = NULL;
    return key;
}

/*
    This functions frees the pointer to the node in the memory binary tree.
    params:
        toBeFreed: the node to be freed. This is always a leaf node.
*/
static void
freeMemoryNode(struct memoryNode *toBeFreed) 
{
    struct memoryNode *temp = toBeFreed;

    if (temp == memRoot) {
        free(temp);
        memRoot = NULL;
        return;
    }

    struct memoryNode *parent = toBeFreed->parent;
    
    if (parent != NULL) {
        if (parent->right == toBeFreed)
            parent->right = NULL;
        else
            parent->left = NULL;
    }

    free(temp);
    
    // If both the children have become NULL, then remove the parent as well..
    temp = parent;
    while ( !(temp->right) && !(temp->left) ) {
        parent = temp->parent;
        if (parent) {
            if (parent->right == temp)
                parent->right = NULL;
            else
                parent->left = NULL;

            free(temp);
            temp = parent;
        } else {
            // We reached root..
            free(temp);
            memRoot = NULL;
            break;
        }
    }

}

static void
freeSHMRegion(int shmid, int processId) {
    if (shmctl(shmid, IPC_RMID, 0) == -1)
            fprintf(stderr, "=> %s: error removing shm for process with id %d: %m. errno: %d.\n", gargv[0], processId, errno);
}

static void
updateGlobalMemVariables(struct queuedProcess *qp) {
    latestMemAllocation = buddyAllocatorWrapper(qp->pcb->process->mem, qp->pcb->nearestPower);
}


/*
    This function creates a new node when a new process arrives, fills in the data, and returns it.
    For explanation regarding the pointer q, refer to the headers.h file. 
*/
static struct queuedProcess* 
newQueuedProcess(struct process *received) 
{

    // Creating a new node for the newly arrived process, and setting the values.                    
    struct queuedProcess *new = malloc(sizeof(struct queuedProcess));
    new->pcb = malloc(sizeof(struct pcb));
    *(new->pcb) = INIT_PCB;
    new->pcb->process = malloc(sizeof(struct process));
    *new->pcb->process = (struct process){received->id, received->arrival, received->runtime, received->priority, received->mem};
    new->pcb->rt = received->runtime;
    new->pcb->nearestPower = nearestPowerOfTwo(new->pcb->process->mem);
    new->next = new->previous = NULL;

    /* 
        Setting the pointer q to the queueing criteria depending on the algorithm type.
        1. When algo = 0, this is HPF. q points to the priority of the process.
        2. When algo = 1, this is SRTN, q points to the remaining time of the process.
        3.

        This is done, so that below in the function insertIntoReadyOrWaitingQ, we can insert a new item in a sorted manner,
        generically, without rewriting the code each time depending on the algorithm type.
        So, we sort from the smallest q (whatever it may be) to the largest one.

    */
    if (algo == 0)
        new->pcb->q = &new->pcb->process->priority;
    else if (algo == 1)
        new->pcb->q = &new->pcb->rt;

    return new;

}


static void
insertIntoWaitingQ(struct queuedProcess *toInsert)
{
    if (!waitingHead) {
        waitingHead = toInsert;
        waitingHead->next = waitingHead->previous = NULL;
        waitingLast = waitingHead;
    } else {
    
        // In case of round robin, append to the end immediately. no questions.
        if (algo == 2) {
            toInsert->next = NULL;
            toInsert->previous = waitingLast;
            waitingLast->next = toInsert;
            waitingLast = toInsert;
            return;
        }

        if (*toInsert->pcb->q < *waitingHead->pcb->q) {

            toInsert->next = waitingHead;
            toInsert->previous = NULL;
            waitingHead->previous = toInsert;
            waitingHead = toInsert;

        } else if (*toInsert->pcb->q >= *waitingLast->pcb->q) {

            toInsert->next = NULL;
            toInsert->previous = waitingLast;
            waitingLast->next = toInsert;
            waitingLast = toInsert;
        } else {
            struct queuedProcess *i = waitingHead;
            for (; *i->pcb->q <= *toInsert->pcb->q; i = i->next);
            
            i->previous->next = toInsert;
            toInsert->previous = i->previous;

            i->previous = toInsert;
            toInsert->next = i;
        }
    }
}

static void 
insertIntoReadyQ(struct queuedProcess *toInsert) 
{

    if (!readyHead) {
        readyHead = toInsert;
        readyHead->next = readyHead->previous = NULL;
        readyLast = readyHead;
    } else {
    
        // In case of round robin, append to the end immediately. no questions.
        if (algo == 2) {
            toInsert->next = NULL;
            toInsert->previous = readyLast;
            readyLast->next = toInsert;
            readyLast = toInsert;
            return;
        }

        if (*toInsert->pcb->q < *readyHead->pcb->q) {

            toInsert->next = readyHead;
            toInsert->previous = NULL;
            readyHead->previous = toInsert;
            readyHead = toInsert;

        } else if (*toInsert->pcb->q >= *readyLast->pcb->q) {

            toInsert->next = NULL;
            toInsert->previous = readyLast;
            readyLast->next = toInsert;
            readyLast = toInsert;


        } else {
        
            struct queuedProcess *i = readyHead;
            for (; *i->pcb->q <= *toInsert->pcb->q; i = i->next);

            i->previous->next = toInsert;
            toInsert->previous = i->previous;

            i->previous = toInsert;
            toInsert->next = i;
        }
    }
}

/*
    This function is used to free the memory blocks associated with the process terminated.
*/
static void 
freeFinishedProcess() 
{
    freeMemoryNode(currentlyRunning->pcb->memoryNode);
    freeSHMRegion(currentlyRunning->pcb->shmid, currentlyRunning->pcb->process->id);

    free(currentlyRunning->pcb->process);
    free(currentlyRunning->pcb);
    free(currentlyRunning);
    currentlyRunning = NULL;
}

/*
    This function is used to fork a new process (the one pointed to by currentlyRunning), and
    passes any required arguments.

    The argument passed:
        argv[1] : the running time.
        argv[2] : the alllocated memory size.
*/
static void 
forkNewProcess() 
{

    int numOfRemainingTimeChars, numOfMemorySizeChars;
    if (currentlyRunning->pcb->process->runtime == 0)
        numOfRemainingTimeChars = 2;
    else 
        numOfRemainingTimeChars = (int)((ceil(log10(currentlyRunning->pcb->process->runtime))+1));
    
    numOfMemorySizeChars = (int)((ceil(log10(currentlyRunning->pcb->process->mem))+1));

    char *remainingTime = malloc( numOfRemainingTimeChars *sizeof(char));
    char *memory = malloc( numOfMemorySizeChars *sizeof(char));

    sprintf(remainingTime, "%d", currentlyRunning->pcb->process->runtime);
    sprintf(memory, "%d", currentlyRunning->pcb->process->mem);

    execlp("./process", "./process", remainingTime, memory, (char *)NULL);
    fprintf(stderr, "=> %s: execlp of ./process failed: %m. errno: %d\n", gargv[0], errno);
}

/*
    This function is called to stop the current process running by sending it a SIGTSTP signal.
*/
static void 
stopCurrentProcess() 
{
    if (currentlyRunning) {
        kill(currentlyRunning->pcb->pid, SIGTSTP);
        do {
            r = read(processFifo, &signalAck, sizeof(int));
        } while (r != sizeof(int) || signalAck != -1);

        clk = getClk();
        
        // Updating the related info
        currentlyRunning->pcb->state = 'T';
        currentlyRunning->pcb->rt -= ( clk - currentlyRunning->pcb->lrt);
        currentlyRunning->pcb->lst = clk;

        fprintf(logger, "At time %d process %d stopped arr %d total %d remain %d wait %d\n", 
                                clk, 
                                currentlyRunning->pcb->process->id, 
                                currentlyRunning->pcb->process->arrival, 
                                currentlyRunning->pcb->process->runtime, 
                                currentlyRunning->pcb->rt, 
                                currentlyRunning->pcb->wt);
    }
}

static void 
removeFromReadyQ(struct queuedProcess *qp) 
{
    
    if (readyHead == readyLast) {
        readyHead = readyLast = NULL;
    } else if (qp == readyHead) {
        readyHead = readyHead->next;
        readyHead->previous = NULL;
    } else if (qp == readyLast) {
        readyLast = readyLast->previous;
        readyLast->next = NULL;
    } else {
        struct queuedProcess* temp = qp->previous;
        temp->next = qp->next;
        qp->next->previous = temp;
    }

    qp->next = qp->previous = NULL;
}

static void
removeFromWaitingQ(struct queuedProcess *qp)
{
    if (waitingHead == waitingLast) {
        waitingHead = waitingLast = NULL;
    } else if (qp == waitingHead) {
        waitingHead = waitingHead->next;
        waitingHead->previous = NULL;
    } else if (qp == waitingLast) {
        waitingLast = waitingLast->previous;
        waitingLast->next = NULL;
    } else {
        struct queuedProcess* temp = qp->previous;
        temp->next = qp->next;
        qp->next->previous = temp;
    }

    qp->next = qp->previous = NULL;
}

/*
    This function is called either to:
    
    1. Start a process for the first time (forking happens in this case).
    2. Resume a stopped process.

    It decides which based on the currentlyRunning->pcb->state, refer to headers.h for meaning of values.

    - It separates the currentlyRunning from the queue, and shifts the readyHead to the next
      process. 

*/
static void startOrResumeHead()
{

    if (readyHead == NULL)
        return;

    currentlyRunning = readyHead;

    removeFromReadyQ(currentlyRunning);

    // If it's stopeed, continue it; if it's a new one, fork.
    if (currentlyRunning->pcb->state == 'T') {
        
        kill(currentlyRunning->pcb->pid, SIGCONT);
        do {
            r = read(processFifo, &signalAck, sizeof(int));
        } while (r != sizeof(int) || signalAck != 1);

        clk = getClk();

        currentlyRunning->pcb->state = 'R';
        currentlyRunning->pcb->wt += clk - currentlyRunning->pcb->lst;
        currentlyRunning->pcb->lrt = clk;
        fprintf(logger, "At time %d process %d resumed arr %d total %d remain %d wait %d\n", 
            clk, 
            currentlyRunning->pcb->process->id, 
            currentlyRunning->pcb->process->arrival, 
            currentlyRunning->pcb->process->runtime, 
            currentlyRunning->pcb->rt, 
            currentlyRunning->pcb->wt
            );
    } else {

        pid = fork();
        switch(pid) {
            case 0:
                
                // closing fifos..
                close(processArrivalFifo);
                close(eoArrivalFifo);
                close(processFifo);

                forkNewProcess();
                break;
            case -1:
                fprintf(stderr, "=> %s: error happened forking a new process: %m. errno: %d\n", gargv[0], errno);
                break;
            default:
                clk = getClk();
                currentlyRunning->pcb->state = 'R';
                currentlyRunning->pcb->pid = pid;
                currentlyRunning->pcb->st = clk;
                currentlyRunning->pcb->lrt = currentlyRunning->pcb->st;
                currentlyRunning->pcb->wt = clk - currentlyRunning->pcb->process->arrival;

                fprintf(logger, "At time %d process %d started arr %d total %d remain %d wait %d\n", 
                            clk, 
                            currentlyRunning->pcb->process->id, 
                            currentlyRunning->pcb->process->arrival, 
                            currentlyRunning->pcb->process->runtime, 
                            currentlyRunning->pcb->process->runtime, 
                            currentlyRunning->pcb->wt
                            );

                // Send them the key!
                char *filePath = malloc(512 * sizeof(char)); 
                int ack;
                snprintf(filePath, 512, "/tmp/%ld", (long)pid);
                
                
                key_t key = generateShmKey(filePath);
                for (int i = 0; i < 3 && key == -1; i++) {
                    key = generateShmKey(filePath);
                }

                if (key != -1) {
                    
                    int shmid = shmget(key, currentlyRunning->pcb->process->mem, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
                    if (shmid == -1) {
                        fprintf(stderr, "=> %s: error creating shared memory region\n", gargv[0]);
                    }

                    write(shmFifo, &key, sizeof(key));

                    // This read blocks till the process reads the key and acknowledge it.
                    r = read(processFifo, &ack, sizeof(ack));
                    
                    if (r == -1) {
                        fprintf(stderr, "=> %s: error receiving acknowledgement for the key by process: %ld\n", gargv[0], (long)pid);
                    }
                    else
                        if (ack == -1)
                            fprintf(stderr, "=> %s: process %ld had an error receiving the key\n", gargv[0], (long)pid);
                        else {
                            currentlyRunning->pcb->shmKey = key;
                            currentlyRunning->pcb->shmid = shmid;
                        }

                    fprintf(stderr, "=> %s: spawned a new process %ld successfully. id: %d\n", gargv[0], (long)pid, currentlyRunning->pcb->process->id);

                } else {
                    fprintf(stderr, "=> %s: failed to generate shm key: %m. errno: %d\n", gargv[0], errno);
                }

                break;
        }
    }
}

/*
    This function is called when a process notifies us it finished.
    It logs to the scheduler.log file all the related info, and then calls the freeFinished() function.
*/
static void
terminateProcess() 
{
    pidOfFinished = malloc(MAX_PID_LENGTH + 1);
    r = read(processFifo, pidOfFinished, MAX_PID_LENGTH + 1);

    if (r == -1) {
        fprintf(stderr, "=> %s: error while reading from processFifo: %m. errno: %d. exiting..", gargv[0], errno);
        exit(EXIT_FAILURE);
    } else if (r == 0) {

        /*  
            This case will never happen. 
            Because the scheduler has opened the FIFO for both reading and writing, it'll never see EOF.
            But here we are, writing it anyway xd
        */
        fprintf(stderr, "=> %s: the unexpected happened. hallelujah.\n", gargv[0]);
        FD_CLR(processFifo, &readFds);
        isProcessFifoEOF = 1;

        if (close(processFifo) == -1)
            fprintf(stderr, "=> %s: error while closing processFifo: %m. errno: %d\n", gargv[0], errno);
        
    } else {

        pid_t terminatedPid;

        if ((terminatedPid = strtol(pidOfFinished, NULL, 0)) != currentlyRunning->pcb->pid) {
            fprintf(stderr, "=> %s: unexpected pid of terminating process! expecting: %ld, got: %ld.\n", gargv[0], (long)currentlyRunning->pcb->pid, (long)terminatedPid);
        }

        waitpid(terminatedPid, NULL, 0);
        clk = getClk();
        currentlyRunning->pcb->ft = clk;


        // Calculating TA and WTA
        ta = currentlyRunning->pcb->ft - currentlyRunning->pcb->process->arrival;
        if (ta == 0 && currentlyRunning->pcb->process->runtime == 0)
            wta = 1;
        else
            wta = ta / currentlyRunning->pcb->process->runtime;
        
        fprintf(logger, "At time %d process %d finished arr %d total %d remain %d wait %d TA %d WTA %.2f\n", clk, 
                    currentlyRunning->pcb->process->id, 
                    currentlyRunning->pcb->process->arrival, 
                    currentlyRunning->pcb->process->runtime, 
                    0,
                    currentlyRunning->pcb->wt,
                    (int)ta,
                    wta);

        fprintf(memoryLogger, "At time %d freed %d bytes for process %d from %d to %d\n", getClk(), currentlyRunning->pcb->process->mem, currentlyRunning->pcb->process->id, currentlyRunning->pcb->memoryNode->start, currentlyRunning->pcb->memoryNode->start + currentlyRunning->pcb->nearestPower);

        // Updating metrics for the scheduler.perf file.
        totalWTA += wta;
        totalWTASquared += pow(wta, 2);
        numOfProcess++; 
        totalWaiting += currentlyRunning->pcb->wt;
        totalAvailMem += currentlyRunning->pcb->nearestPower;

        freeFinishedProcess();
    }

    free(pidOfFinished);
}

/*
    This function is called to switch processes running.
    It stops the current process (if there is any), and then starts (or resumes) the readyHead
    of the queue of the rest of (yet to run, or stopped) processes.

    Return values:
    0 => means no switching has occurred (because there's nothing queued).
    1 => means switching has occured.

    Parameters: terminateCurrent
    1 => terminate the current process.
    0 => stop the current process.

*/
static int switchProcess(int terminateCurrent) {

    // If asked to stop the current process, but there's nothing queued, do nothing and return 0.
    if (!readyHead && !terminateCurrent) {
        return 0;
    }

    struct queuedProcess *stopped = currentlyRunning;
    if (stopped) {

        if (!terminateCurrent) {
            stopCurrentProcess();
            insertIntoReadyQ(stopped);
        } else {
            terminateProcess();
        }

    } else {
        cpuIdleTime += getClk() - lastIdle;
    }

    startOrResumeHead();
    return 1;
}

/*
    This function is called when a process arrives.
    It reads the process from the processArrivalFifo, and then writes and acknowledgement 
    arbitrary int value to the ackFifo;

    It then reads an int value from the eoArrivalFifo, and returns it,
    if this value is 
    1 => it means there are more processes coming at the same arrival time
    0 => it means there's no more processes to come at this arrival time.
*/
static int
processArrived() 
{
    struct process *received = malloc(sizeof(struct process));

    r = read(processArrivalFifo, received, sizeof(struct process));

    if (r == -1) {
        fprintf(stderr, "=> %s: error while reading from processArrivalFifo: %m. errno: %d. exiting..", gargv[0], errno);
        exit(EXIT_FAILURE);
    } else if (r == 0) {
        
        fprintf(stderr, "=> %s: received EOF from the process generator.\n", gargv[0]);
        
        FD_CLR(processArrivalFifo, &readFds);
        isProcessArrivalFifoEOF = 1;
        

        // Closing all the FIFOs.
        if (close(processArrivalFifo) == -1)
            fprintf(stderr, "=> %s: error while closing processArrivalFifo: %m. errno: %d\n", gargv[0], errno);
        else
            processArrivalFifo = -1;

        if (close(eoArrivalFifo) == -1)
            fprintf(stderr, "=> %s: error while closing eoArrivalFifo: %m. errno: %d\n", gargv[0], errno);
        else
            eoArrivalFifo = -1;

        if (close(ackFifo) == -1)
            fprintf(stderr, "=> %s: error while closing ackFifo: %m. errno: %d\n", gargv[0], errno);
        else
            ackFifo = -1;

        return -1;
    } else {

        // Writing to the ackFifo acknowledging the receiving of the process.
        if (write(ackFifo, &(received->id), sizeof(int)) != sizeof(int))
            fprintf(stderr, "error writing!\n");
        
        // Creating a new node for the newly arrived process, and setting the values.                    
        struct queuedProcess *newlyArrived = newQueuedProcess(received);
        

        // Allocate memory...
        updateGlobalMemVariables(newlyArrived);
        if (!latestMemAllocation)
            insertIntoWaitingQ(newlyArrived);
        else {
            newlyArrived->pcb->memoryNode = latestMemAllocation;
            
            fprintf(memoryLogger, "At time %d allocated %d bytes for process %d from %d to %d\n", 
                getClk(), 
                newlyArrived->pcb->process->mem, 
                newlyArrived->pcb->process->id, 
                newlyArrived->pcb->memoryNode->start, 
                newlyArrived->pcb->memoryNode->start + newlyArrived->pcb->nearestPower
            );

            totalAvailMem -= newlyArrived->pcb->nearestPower;
            insertIntoReadyQ(newlyArrived);
        }
                
        totalRuntime += newlyArrived->pcb->process->runtime;

        fprintf(stderr, "=> %s: a process arrived, id: %d, arrival: %d at clk: %d.\n", gargv[0], received->id, received->arrival, getClk());

        if (read(eoArrivalFifo, &moreOrNone, sizeof(int)) != sizeof(int))
            fprintf(stderr, "=> %s: error happened while reading from eoArrivalFifo: %m. errno: %d.\n", gargv[0],errno);

        if (moreOrNone == 1) {
            fprintf(stderr, "=> %s: there's still more to come at this arrival time.\n", gargv[0]);
        }

        return moreOrNone;
    }
}

/*
    This function is called whenever a process terminates.
    
    We loop through the waiting queue, and allocate memory for as many processes as we can, and
    move them to the ready queue.

    The process are inserted in the waiting queue in the same order that they are inserted in the ready
    queue.
    That is:
        - In HPF, higher priority to lower priority.
        - IN SRTN, lesser remaining time to longer remaining time.
        - IN RR, as they arrived.

    So, for example IN HPF, whenever there's available memory, we do allocate memory for the highest priority
    processes first, but if there's not enough memory for them, and we can allocate memory for ones with lower
    priority, we do so.

*/
static void 
allocateAllYouCan()
{

    struct queuedProcess *iterator = waitingHead;
    while (iterator) {
        updateGlobalMemVariables(iterator);
        if (latestMemAllocation) {

            struct queuedProcess *temp = iterator->next;

            // allocate and move to the ready queue...
            iterator->pcb->memoryNode = latestMemAllocation;
            totalAvailMem -= iterator->pcb->nearestPower;

            // Log the allocation!
            fprintf(memoryLogger, "At time %d allocated %d bytes for process %d from %d to %d\n", 
                getClk(), 
                iterator->pcb->process->mem, 
                iterator->pcb->process->id, 
                iterator->pcb->memoryNode->start, 
                iterator->pcb->memoryNode->start + iterator->pcb->nearestPower
            );

            // Remove from waiting and insert into ready..
            removeFromWaitingQ(iterator);
            insertIntoReadyQ(iterator);

            iterator = temp;
        } else 
            iterator = iterator->next;
    }
}

static void
closeFifo(int fifoFd)
{
    if (fifoFd != -1)
        if (close(fifoFd) == -1)
            fprintf(stderr, "=> %s: error closing a fifo: %m. errno: %d\n", gargv[0], errno);
}

static void 
clearResources(int signum)
{
    struct queuedProcess *q, *i;
    struct queuedProcess *setOfHeads[2] = {readyHead, waitingHead}; 

    // Freeing the processes in both queues: ready queue, and waiting queue.
    for (int j = 0; j < 2; j++) {
        i = setOfHeads[j];
        while (i) {
            
            /*
                Normal termination -> all processes have been waited on. (The queues are already empty)
                Interrupted -> any process that is in the ready queue shall be waited on, they were already interrupted by the signal
                    sent through the terminal.

                Either way: wait.
            */

            // Waiting on processes if they are created and stopped, or are running.
            if (i->pcb->state == 'T' || i->pcb->state == 'R') {
                if (waitpid(i->pcb->pid, NULL, 0) == -1)
                    fprintf(stderr, "=> %s: failed to wait on process with pid %d: %m. errno: %d\n", gargv[0], i->pcb->pid, errno);
            
                freeSHMRegion(i->pcb->shmid, i->pcb->process->id);
            }

            // Freeing memory nodes.
            if (i->pcb->memoryNode)
                freeMemoryNode(i->pcb->memoryNode);

            free(i->pcb->process);
            free(i->pcb);
            
            q = i;
            i = i->next;
            free(q);
            q = NULL;
        }
    }


    if (currentlyRunning) {

        if (waitpid(currentlyRunning->pcb->pid, NULL, 0) == -1)
            fprintf(stderr, "=> %s: failed to wait on process with pid %d: %m. errno: %d\n", gargv[0], currentlyRunning->pcb->pid, errno);

        freeMemoryNode(currentlyRunning->pcb->memoryNode);
        freeSHMRegion(currentlyRunning->pcb->shmid, currentlyRunning->pcb->process->id);

        free(currentlyRunning->pcb->process);
        free(currentlyRunning->pcb);
        free(currentlyRunning);
    }

    // Closing the FIFOs.
    closeFifo(eoArrivalFifo);
    closeFifo(processArrivalFifo);
    closeFifo(processFifo);
    closeFifo(shmFifo);
    closeFifo(ackFifo);
    
    // Closing all the logging files.
    if (logger)
        fclose(logger);
    
    if (metrics)
        fclose(metrics);

    if (memoryLogger)
        fclose(memoryLogger);

    // Deleting the file we used to generate the SHM keys.
    unlink(shmKeyFileGen);
    
    // Detaching the shmaddr.
    destroyClk();

    exit(EXIT_SUCCESS);
}

/*
    HPF-related.
*/

static void 
HPF() 
{
    while(true) {

        /* 
            We break from this loop, when the following are satisifed:
            1. There's no process running.
            2. There are no processes queued.
            3. The process generator has closed its write end of processArrivalFifo (meaning there are no more processes to arrive). 
        */
        if (!readyHead && !currentlyRunning && isProcessArrivalFifoEOF) {
            fprintf(stderr, "=> %s: nothing running, nothing queued, processArrivalFifo closed. WE SHALL ALSO CLOSE.\n", gargv[0]);
            break;
        }
        
        /*
            This select() call blocks waiting to read on one of the following, or both, occur:
            1. The processArrivalFifo (a new process has arrived).
            2. The processFifo (a process has terminated and finished running).
        */

        selectReturn = select(nfds, &readFds, NULL, NULL, NULL);
        if (selectReturn == -1)
            fprintf(stderr, "=> %s: error selecting: %m. errno: %d\n", gargv[0], errno);


        /*
            This if case occurs when a process has terminated, and sent notifying us.
        */
        if (FD_ISSET(processFifo, &readFds)) {
            terminateProcess();
            allocateAllYouCan();
            lastIdle = getClk();
        } else {
            if (!isProcessFifoEOF)
                FD_SET(processFifo, &readFds);
        }

        /*
            This if case occurs when a new process has arrived, sent by the process generator to the processArrivalFifo.
            Note: these two if cases can occur together, that's why we're checking for them separately.
        */
        if (FD_ISSET(processArrivalFifo, &readFds)) {
            if (processArrived() == 0) {
                if (!currentlyRunning) {
                    switchProcess(1);
                }
            } else
                continue;
        } else {
            if (!isProcessArrivalFifoEOF)
                FD_SET(processArrivalFifo, &readFds);
        }

        if (!currentlyRunning && readyHead)
            switchProcess(1);

    }
}
/*
    SRTN-related.
*/

static void 
SRTN()
{
    while(true) {

        if (!readyHead && !waitingHead && !currentlyRunning && isProcessArrivalFifoEOF) {
            fprintf(stderr, "=> %s: nothing running, nothing queued, processArrivalFifo closed. WE SHALL ALSO CLOSE.\n", gargv[0]);
            break;
        }

        selectReturn = select(nfds, &readFds, NULL, NULL, NULL);
        if (selectReturn == -1)
            fprintf(stderr, "=> %s: error selecting: %m. errno: %d\n", gargv[0], errno);

        if (FD_ISSET(processFifo, &readFds)) {
            terminateProcess();
			allocateAllYouCan();
            lastIdle = getClk();
        } else {
            if (!isProcessFifoEOF)
                FD_SET(processFifo, &readFds);
        }

        /*
            This if case occurs when a new process has arrived, sent by the process generator to the processArrivalFifo.
            Note: these two if cases can occur together, that's why we're checking for them separately.
        */
        if (FD_ISSET(processArrivalFifo, &readFds)) {
            if (processArrived())
                continue;
        } else {
            if (!isProcessArrivalFifoEOF)
                FD_SET(processArrivalFifo, &readFds);
        }

        // If there's nothing running, or if the process running has remaining time greater than
        // the readyHead of the queue, switch and start another process.
        clk = getClk();        
        if ((!currentlyRunning && !readyHead) || (!readyHead))
            continue;
        else if (!currentlyRunning)
           startOrResumeHead();
        else if (currentlyRunning->pcb->rt - (clk - currentlyRunning->pcb->lrt) > readyHead->pcb->rt) {
            stopCurrentProcess();
            insertIntoReadyQ(currentlyRunning);
            startOrResumeHead();
        }
    }
}

/*
    RR-related.
*/

static void 
RR()
{  
    struct timeval timeout;
    timeout.tv_sec = qunatum;
    timeout.tv_usec = 0;

    while(true) {

        if (!readyHead && !waitingHead && !currentlyRunning && isProcessArrivalFifoEOF) {
            fprintf(stderr, "=> %s: nothing running, nothing queued, processArrivalFifo closed. WE SHALL ALSO CLOSE.\n", gargv[0]);
            break;
        }
        selectReturn = select(nfds, &readFds, NULL, NULL, &timeout);
        if (selectReturn == -1)
            fprintf(stderr, "=> %s: error selecting: %m. errno: %d\n", gargv[0], errno);


        if (FD_ISSET(processFifo, &readFds)) {
            terminateProcess();
            allocateAllYouCan();
            lastIdle = getClk();
        } else {
            if (!isProcessFifoEOF)
                FD_SET(processFifo, &readFds);
        }

        if (FD_ISSET(processArrivalFifo, &readFds)) {
            processArrived();
        } else {
            if (!isProcessArrivalFifoEOF)
                FD_SET(processArrivalFifo, &readFds);
        }

        // Check timeout..
        if (timeout.tv_sec == 0 && timeout.tv_usec == 0) {

            int switched = switchProcess(0);
            if (currentlyRunning) {
                if (!switched)
                    hasHadItsQuantum = true;
                else
                    hasHadItsQuantum = false;
            }

            timeout.tv_sec = qunatum;
            timeout.tv_usec = 0;
        }
        
        /*
        
            This case occurs when one of the following is satisfied:
            1. There's nothing running (a process was running and terminated above).
            
            2. There's something running, which has had its quantum, but kept running because the queue was empty.
               And now, the queue is not empty, some new process has arrived, and was inserted in the queue above.
        
        */
        
        else if (!currentlyRunning || (hasHadItsQuantum && readyHead)) {
            switchProcess(0);
            if(!currentlyRunning)
                lastIdle = getClk();

            timeout.tv_usec = 0;
            timeout.tv_sec = qunatum;
            hasHadItsQuantum = false;
        }
    }
}

int main(int argc, char * argv[])
{

    setbuf(stderr, NULL);

    signal(SIGINT, clearResources);

    if (argc < 2) {
        fprintf(stderr, "=> %s: algorithm must be supplied as an argument.\n", argv[0]);
        raise(SIGINT);
    }

    gargv = argv;

    algo = atoi(argv[1]);
    if (algo == 2) {
        if(argc < 3){
            fprintf(stderr, "=> %s: supply the quantum in seconds for the RR algorithm as a third parameter.\n", argv[0]);
            raise(SIGINT);
        }
        else {
            qunatum = atoi(argv[2]);
        }
    }

    // Logging
    logger = fopen("scheduler.log", "w");
    memoryLogger = fopen("memory.log", "w");

    if (!logger) {
        fprintf(stderr, "=> %s: error opening the file scheduler.log.\n", argv[0]);
        raise(SIGINT);
    }

    setbuf(logger, NULL);
    setbuf(memoryLogger, NULL);

    fprintf(logger, "#At time x process y state arr w total z remain y wait k\n");
    fprintf(memoryLogger, "#At time x allocated y bytes for process z from i to j\n");

    if (mkfifo(PROCESS_FIFO, S_IWUSR | S_IRUSR | S_IRGRP) == -1 && errno != EEXIST) {
        fprintf(stderr, "=> %s: error creating the PROCESS_FIFO: %m. errno: %d", argv[0], errno);
        raise(SIGINT);
    }

    if (mkfifo(SHM_FIFO, S_IRUSR | S_IWUSR | S_IRGRP) == -1 && errno != EEXIST) {
        fprintf(stderr, "=> %s: error creating the SHM_FIFO: %m. errno: %d", argv[0], errno);
        raise(SIGINT);
    }

    eoArrivalFifo = open(EOF_ARRIVAL_FIFO, O_RDONLY);
    processArrivalFifo = open(PROCESS_ARRIVAL_FIFO, O_RDONLY);
    ackFifo = open(ACK_FIFO, O_WRONLY);
    processFifo = open(PROCESS_FIFO, O_RDWR);
    shmFifo = open(SHM_FIFO, O_RDWR);


    if (eoArrivalFifo == -1 || processArrivalFifo == -1 || processFifo == -1 || shmFifo == -1 || ackFifo == -1) {
        fprintf(stderr, "=> %s: error opening the one of the FIFOs: %m. errno: %d", argv[0], errno);
        raise(SIGINT);
    }

    // Creating the file /tmp/scheduler-key-process as a backup file for generating keys for the shared memory regions
    // allocated to processes.
    int fileGen = open(shmKeyFileGen, O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP);
    if (fileGen == -1 && errno != EEXIST) {
        fprintf(stderr, "=> %s: error creating the file %s: %m. errno: %d\n", argv[0], shmKeyFileGen, errno);
        clearResources(-1);
    }
    close(fileGen);

    /* 
        Preparing the sets and the arguments for the select() call above in each algorithm's function respectively.
    */

    if (processArrivalFifo > processFifo)
        nfds = processArrivalFifo + 1;
    else
        nfds = processFifo + 1;

    FD_ZERO(&readFds);
    FD_SET(processFifo, &readFds); 
    FD_SET(processArrivalFifo, &readFds);

    latestMemAllocation = malloc(sizeof(struct memoryNode));

    initClk();

    switch (algo) {
        case 0:
            HPF();
            break;
        case 1:
            SRTN();
            break;
        case 2:
            RR();
            break;
        default: 
            fprintf(stderr, "NOT IMPLEMENTED\n");
            break;
    }

    // Generate scheduler.perf
    metrics = fopen("scheduler.perf", "w");
    if (!metrics) {
        fprintf(stderr, "=> %s: error opening the file scheduler.perf.\n", argv[0]);
        raise(SIGINT);
    }

    float avgWTA = (float)totalWTA / (float)numOfProcess;
    float std = (totalWTASquared - numOfProcess * pow(avgWTA, 2)) / (numOfProcess - 1);
    std = sqrt(std);

    fprintf(metrics, "CPU utilization = %.2f%%\n", ((float)totalRuntime / (float) (cpuIdleTime + totalRuntime)) * 100);
    fprintf(metrics, "Avg WTA = %.2f\n", avgWTA);
    fprintf(metrics, "Avg Waiting = %.2f\n", (float)totalWaiting / (float)numOfProcess);

    fprintf(metrics, "Std WTA = %.2f", std);

    clearResources(-1);
}
