/*
 * shared.h — Shared definitions for CS-4760 Project 5
 *
 * Included by both oss.c and worker.c. Defines IPC keys, constants,
 * message action codes, and the data structures that live in shared memory
 * or are passed through the message queue.
 *
 * Memory layout: one shmget segment holds a single SharedMem struct.
 * Only oss writes; workers attach the segment read-only.
 *
 * Message queue: all worker→oss messages use mtype=1 so oss can drain
 * them in a single IPC_NOWAIT loop. Oss→worker messages use mtype=worker_pid
 * so each worker wakes only for its own reply.
 */

#ifndef SHARED_H
#define SHARED_H

#include <sys/types.h>

/* ── Resource & Process Limits ──────────────────────────────────────────────
 * 10 resource classes × 5 instances each = 50 total units in the system.
 * MAX_PROCESSES is the total PCB table size; MAX_RUNNING is the hard cap
 * on simultaneous live workers (-s flag is further capped to this value).
 */
#define RESOURCE_CLASSES    10
#define INSTANCES_PER       5
#define MAX_PROCESSES       20
#define MAX_RUNNING         18

/* ── Simulated Clock ─────────────────────────────────────────────────────────
 * CLOCK_INC_NS is kept for reference; advance_clock() in oss.c now drives
 * the sim clock off real elapsed time (1:1 ratio) rather than a fixed
 * per-iteration increment, so loop speed no longer affects sim speed.
 */
#define CLOCK_INC_NS        10000000     /* 10 ms — kept as a reference constant */

/* ── Periodic Intervals (simulated time) ─────────────────────────────────────
 * Deadlock detection runs once per simulated second.
 * The resource allocation table is printed every 0.5 simulated seconds.
 */
#define DETECT_INTERVAL_S   1            /* sim-seconds between deadlock checks  */
#define PRINT_INTERVAL_NS   500000000    /* 500 ms sim-time between table prints */

/* ── Hard Limits ─────────────────────────────────────────────────────────────
 * MAX_LOG_LINES prevents runaway log files on long or busy runs.
 * SIGALRM_LIMIT is a wall-clock safety valve: if the run is still going at
 * 30 real seconds, SIGALRM fires and sig_handler forces a clean shutdown.
 * WALL_CLOCK_LIMIT is the forking window: no new workers are launched after
 * this many real seconds have elapsed, regardless of -n.
 */
#define MAX_LOG_LINES       10000
#define SIGALRM_LIMIT       30           /* real seconds before forced SIGALRM exit */
#define WALL_CLOCK_LIMIT    5            /* real seconds before forking stops       */

/* ── IPC Keys ────────────────────────────────────────────────────────────────
 * Fixed keys identify the shared memory segment and message queue across
 * fork/exec.  If a run exits uncleanly, remove leftovers with:
 *   ipcrm -m $(ipcs -m | awk '/0x47605/{print $2}')
 *   ipcrm -q $(ipcs -q | awk '/0x47606/{print $2}')
 */
#define SHM_KEY   0x47605   /* shared memory key */
#define MSG_KEY   0x47606   /* message queue key */

/* ── Message Action Codes ────────────────────────────────────────────────────
 * These go in Message.msg_action so the receiver knows the intent of a
 * message even when mvalue alone is ambiguous (e.g., mvalue=1 could mean
 * "resource 0 requested" or just "your turn with payload 1").
 *
 * Direction key:
 *   oss→worker: MSG_TURN, MSG_GRANT, MSG_TERMINATE
 *   worker→oss: MSG_REQUEST, MSG_RELEASE, MSG_TERMINATE
 */
#define MSG_TURN      0   /* oss → worker: take your turn, no resource change      */
#define MSG_GRANT     1   /* oss → worker: your blocked request has been granted   */
#define MSG_REQUEST   2   /* worker → oss: I want one unit of resource (mvalue-1)  */
#define MSG_RELEASE   3   /* worker → oss: I am returning one unit of (−mvalue−1)  */
#define MSG_TERMINATE 4   /* bidirectional: oss orders kill, or worker acks exit   */

/* ── SimClock ────────────────────────────────────────────────────────────────
 * Two-field simulated wall clock.  Only oss increments it; workers read it
 * to check their per-process simulated time limit.
 */
typedef struct {
    unsigned int sec;   /* whole simulated seconds */
    unsigned int ns;    /* nanosecond fraction (0 – 999,999,999) */
} SimClock;

/* ── Process Control Block ───────────────────────────────────────────────────
 * One PCB per process-table slot.  oss owns all writes; workers are
 * read-only on shared memory and never touch the PCB directly.
 */
typedef struct {
    int   occupied;                      /* 1 = slot holds a live process, 0 = free */
    pid_t pid;                           /* OS PID of the worker in this slot        */
    int   allocation[RESOURCE_CLASSES];  /* units of each resource currently held    */
    int   blocked;                       /* 1 = process is waiting for a resource    */
    int   blocked_resource;             /* index of the resource being waited on;
                                          -1 when not blocked                        */
} PCB;

/* ── SharedMem — the single shared memory segment ───────────────────────────
 * Everything oss needs to share with workers lives here.
 *
 * request[i][r] == 1 means process in slot i is currently blocked waiting
 * for one unit of resource r.  This matrix is read by the deadlock detector
 * (Banker's algorithm) without additional IPC.
 *
 * available[] tracks how many unallocated units of each resource exist.
 * available[r] + sum(allocation[i][r]) == INSTANCES_PER at all times.
 */
typedef struct {
    SimClock clock;                                  /* simulated system clock       */
    PCB      proctable[MAX_PROCESSES];               /* process control block table  */
    int      available[RESOURCE_CLASSES];            /* free units per resource      */
    int      request[MAX_PROCESSES][RESOURCE_CLASSES]; /* blocked-request matrix     */
} SharedMem;

/* ── IPC Message ─────────────────────────────────────────────────────────────
 * Sent through the System V message queue in both directions.
 *
 * mvalue encoding (resource indices are 1-based to keep 0 unambiguous):
 *   mvalue > 0  — resource index is (mvalue − 1); used for requests and grants
 *   mvalue < 0  — resource index is (−mvalue − 1); used for releases
 *   mvalue == 0 — terminate signal or acknowledge
 *
 * sender_slot is filled by the worker so oss does not have to scan the PCB
 * table to map the incoming PID to a slot index.
 */
typedef struct {
    long mtype;        /* routing: 1 for oss-bound; worker_pid for worker-bound */
    int  mvalue;       /* resource operand (see encoding above)                 */
    int  sender_slot;  /* PCB slot of the sending worker (worker fills this)    */
    int  msg_action;   /* one of the MSG_* constants above                      */
} Message;

#endif /* SHARED_H */
