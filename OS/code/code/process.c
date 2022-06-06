#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/shm.h>

int remainingtime, processFifo = -1, shmFifo = -1, shmid = -1, memorySize;
ssize_t len;
pid_t myPid;
char *myMem = NULL;

static void setHandler(int signum);

#define PROCESS_FIFO "/tmp/process_fifo"
#define SHM_FIFO "/tmp/process_shm_fifo"
#define MAX_PID_LENGTH 12

static void
SIGCONTHandler(int sig, siginfo_t *siginfo, void *ucontext)
{
    if (siginfo->si_pid == getppid()) {
        fprintf(stderr, "=> process %ld: parent continued me.\n", (long)myPid);
        signal(SIGCONT, SIG_DFL);
        
        // Reset signal handler for the SIGTSTP
        setHandler(SIGTSTP);

        // Acknowledge stopping..
        int x = 1;
        write(processFifo, &x, sizeof(int));

        // Raising SIGCONT again once the two handlers have been established.
        raise(SIGCONT);
    } else {
        raise(SIGTSTP);
    }
}

static void
SIGTSTPHandler(int sig, siginfo_t *siginfo, void *ucontext)
{

    if (siginfo->si_pid == getppid()) {
        
        fprintf(stderr, "=> process %ld: parent stopped me.\n", (long)myPid);
        signal(SIGTSTP, SIG_DFL);
        setHandler(SIGCONT);

        // Acknowledge stopping..
        int x = -1;
        write(processFifo, &x, sizeof(int));

        // Raising SIGTSTP again to stop the process (the signal handler has been restored to default).
        raise(SIGTSTP);
    }
}

static void
SIGINTHandler(int sig, siginfo_t *siginfo, void *ucontext) 
{

    fprintf(stderr, "=> process %ld: just got interrupted. cleaning and exiting.\n", (long)myPid);
    
    // Cleaning resources..
    if (processFifo != -1)
        close(processFifo);

    if (shmFifo != -1)
        close(shmFifo);

    if (myMem)
        shmdt(myMem);

    exit(EXIT_SUCCESS);

}

static void
setHandler(int signum) 
{
    struct sigaction sa;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;

    switch (signum) {
        case SIGINT:
            sa.sa_sigaction = SIGINTHandler;
            break;

        case SIGCONT:
            sa.sa_sigaction = SIGCONTHandler;
            break;

        case SIGTSTP:
            sa.sa_sigaction = SIGTSTPHandler;
    }

    if (sigaction(signum, &sa, NULL) == -1)
        fprintf(stderr, "=> process %ld: error setting the handler for signal: %d.\n", (long)myPid, signum);
}

int main(int argc, char * argv[])
{

    if (argc < 3)
        exit(EXIT_FAILURE);

    myPid = getpid();

    setHandler(SIGINT);
    setHandler(SIGTSTP);

    remainingtime = atoi(argv[1]);
    memorySize = atoi(argv[2]);

    struct timeval timeout;
    timeout.tv_sec = remainingtime;
    timeout.tv_usec = 0;

    processFifo = open(PROCESS_FIFO, O_WRONLY);
    shmFifo  = open(SHM_FIFO, O_RDONLY);

    if (processFifo == -1) {
        fprintf(stderr, "=> process %ld: error opening  the process fifo: %m. errno: %d\n", (long)myPid, errno);
        exit(EXIT_FAILURE);
    } else if (shmFifo == -1) {
        fprintf(stderr, "=> process %ld: error opening the acknowledgement fifo: %m. errno: %d\n", (long)myPid, errno);
        exit(EXIT_FAILURE);
    }
 
    key_t shmkey; int ack = -1;
    
    // Let's put a timeout on this read!
    fd_set readFds; int nfds = shmFifo + 1;  
    
    FD_ZERO(&readFds);
    FD_SET(shmFifo, &readFds);
    
    struct timeval readTimeout;
    readTimeout.tv_sec = 1;

    int selectReturn = select(nfds, &readFds, NULL, NULL, &readTimeout);
    if (selectReturn == -1) {
    } else {

        len = read(shmFifo, &shmkey, sizeof(shmkey));
        
        switch (len) {
            case 0:
                fprintf(stderr, "=> process %ld: received EOF on \n", (long)myPid);
                write(processFifo, &ack, sizeof(ack));
                exit(EXIT_FAILURE);
            case -1:
                fprintf(stderr, "=> process %ld: error happened reading the shm key: %m. errno: %d.\n", (long)myPid, errno);
                write(processFifo, &ack, sizeof(ack));
                exit(EXIT_FAILURE);
            default: {
                fprintf(stderr, "=> process %ld: received key: %d\n", (long)myPid, shmkey);
                ack = 1;
                write(processFifo, &ack, sizeof(ack));
                break;
            }
        }

        shmid = shmget(shmkey, memorySize, 0);

        // Allocate the shared memory (you got the key)..
        if (shmid == -1)
            fprintf(stderr, "=> process %ld: error getting shmid: %m. errno: %d.\n", (long)myPid, errno);
        else {
            myMem = (char *) shmat(shmid, NULL, 0);
            if (myMem == NULL)
                fprintf(stderr, "=> process %ld: error attaching to the shm: %m. errno: %d.\n", (long)myPid, errno);
        }
    }

    int ret;
    while (timeout.tv_sec != 0 || timeout.tv_usec != 0)
    {
        ret = select(0, NULL, NULL, NULL, &timeout);
        
        if (ret == -1) {
            if (errno == EINTR) {
                fprintf(stderr, "=> process %ld: waking up.. remaining time = %ld seconds and %ld microseconds.\n", (long)myPid, timeout.tv_sec, timeout.tv_usec);
            } else
                fprintf(stderr, "=> process %ld: error from select(): %m. errno: %d\n", (long)myPid, errno);
        }
    }

    // Notify the parent that I finished...
    char *pid = malloc(MAX_PID_LENGTH + 1);
    sprintf(pid, "%ld", (long)getpid());
    write(processFifo, pid, MAX_PID_LENGTH + 1);


    fprintf(stderr, "=> process %ld: terminating...\n", (long)myPid);
    free(pid);
    close(processFifo);

    return 0;
}
