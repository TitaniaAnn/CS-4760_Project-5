#ifndef SHARED_H
#define SHARED_H

#include <sys/types.h>

/* ── Resource & Process Limits ─────────────────────────────────── */
#define RESOURCE_CLASSES    10
#define INSTANCES_PER       5
#define MAX_PROCESSES       20
#define MAX_RUNNING         18

/* ── Simulated Clock ────────────────────────────────────────────── */
#define CLOCK_INC_NS        10000000     /* 10 ms per main-loop tick  */

/* ── Periodic Intervals (simulated nanoseconds / seconds) ─────── */
#define DETECT_INTERVAL_S   1            /* deadlock check every 1 sim-sec  */
#define PRINT_INTERVAL_NS   500000000    /* table print every 0.5 sim-sec   */

/* ── Hard Limits ────────────────────────────────────────────────── */
#define MAX_LOG_LINES       10000
#define SIGALRM_LIMIT       30           /* wall-clock seconds before forced exit */
#define WALL_CLOCK_LIMIT    5            /* wall-clock seconds before stop forking */

/* ── IPC Keys ───────────────────────────────────────────────────── */
#define SHM_KEY   0x47605
#define MSG_KEY   0x47606

/* ── Message Action Codes ───────────────────────────────────────── */
#define MSG_TURN      0   /* oss → worker: your turn, no resource action      */
#define MSG_GRANT     1   /* oss → worker: resource granted                   */
#define MSG_REQUEST   2   /* worker → oss: requesting one unit of a resource  */
#define MSG_RELEASE   3   /* worker → oss: releasing one unit of a resource   */
#define MSG_TERMINATE 4   /* bidirectional: terminate signal / ack             */

/* ── Simulated Clock ────────────────────────────────────────────── */
typedef struct {
    unsigned int sec;
    unsigned int ns;
} SimClock;

/* ── Process Control Block ──────────────────────────────────────── */
typedef struct {
    int   occupied;                        /* 1 = slot in use              */
    pid_t pid;
    int   allocation[RESOURCE_CLASSES];    /* units currently held          */
    int   blocked;                         /* 1 = waiting on a resource     */
    int   blocked_resource;                /* which resource (-1 if none)   */
} PCB;

/* ── Shared Memory Segment ──────────────────────────────────────── */
typedef struct {
    SimClock clock;
    PCB      proctable[MAX_PROCESSES];
    int      available[RESOURCE_CLASSES];
    /* request[i][r] == 1 means process i is blocked waiting for resource r */
    int      request[MAX_PROCESSES][RESOURCE_CLASSES];
} SharedMem;

/* ── IPC Message ────────────────────────────────────────────────── */
typedef struct {
    long mtype;         /* routing: 1 for oss-bound; worker_pid for worker-bound */
    int  mvalue;        /* +N = resource N-1 (request or grant)                  */
                        /* -N = resource N-1 (release)                           */
                        /*  0 = terminate                                        */
    int  sender_slot;   /* process-table slot of sender (worker fills this)      */
    int  msg_action;    /* MSG_TURN / MSG_GRANT / MSG_REQUEST / MSG_RELEASE / MSG_TERMINATE */
} Message;

#endif /* SHARED_H */
