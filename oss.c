/*
 * oss.c — OS Simulator: master resource manager and deadlock detector
 *
 * CS-4760 Project 5
 * Author: Cynthia Sims
 *
 * Responsibilities:
 *   - Maintains a simulated clock in shared memory (sec + ns).
 *   - Forks up to -n worker processes, capped at -s simultaneous.
 *   - Dispatches "turns" to workers round-robin via a System V message queue.
 *   - Grants or queues resource requests; satisfies queued waiters on release.
 *   - Runs Banker's-algorithm deadlock detection every simulated second and
 *     terminates one victim at a time until the deadlock set is clear.
 *   - Prints a resource allocation table every 0.5 simulated seconds.
 *   - Cleans up all IPC on normal exit, SIGINT, SIGTERM, and SIGALRM.
 *
 * Message protocol (see shared.h for full encoding):
 *   worker → oss : mtype = 1  (all workers share this type so oss can drain
 *                               the queue with a single IPC_NOWAIT loop)
 *   oss → worker : mtype = worker_pid  (each worker wakes only for its own reply)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include "shared.h"

/* ════════════════════════════════════════════════════════════════════════════
   Global state
   ════════════════════════════════════════════════════════════════════════════ */

/* Pointers to the attached shared memory segment and IPC identifiers.
 * Initialized to sentinel values so cleanup_ipc() can safely skip
 * resources that were never successfully created. */
static SharedMem *shm   = NULL;
static int        shmid = -1;
static int        msgid = -1;

/* Per-resource FIFO wait queues (oss-private; not in shared memory).
 * When a request cannot be immediately granted, the requester's slot is
 * enqueued here.  The queues are circular arrays of size MAX_PROCESSES. */
static int wq[RESOURCE_CLASSES][MAX_PROCESSES]; /* slot indices waiting per resource */
static int wq_head[RESOURCE_CLASSES];           /* dequeue index for each resource   */
static int wq_tail[RESOURCE_CLASSES];           /* enqueue index for each resource   */
static int wq_size[RESOURCE_CLASSES];           /* current occupancy of each queue   */

/* Round-robin turn dispatcher state.
 * rr_index cycles through all slots; dispatch_one_turn() sends a MSG_TURN
 * to the next eligible (occupied, unblocked, not already waiting) worker.
 *
 * turn_sent[i] == 1 means oss has sent a message to slot i and is waiting
 * for a reply.  This prevents flooding a single worker with turns before
 * it has a chance to respond.  Cleared when the worker's reply arrives. */
static int rr_index              = 0;
static int turn_sent[MAX_PROCESSES];

/* Process accounting used to enforce launch limits and detect completion. */
static int total_launched    = 0;  /* total workers ever forked this run      */
static int currently_running = 0;  /* workers currently alive                 */

/* Configuration — set from command-line options, then read-only. */
static int          n_limit       = 20;          /* -n: total workers to launch     */
static int          s_limit       = 5;           /* -s: max simultaneous workers    */
static int          t_limit_s     = 3;           /* -t: sim-time limit per worker (s) */
static unsigned int launch_int_ns = 100000000u;  /* -i: sim-ns between launches (100ms) */
static char         logfile[256]  = "oss.log";   /* -f: output log path             */

/* Simulated-time markers for periodic operations.
 * Each pair (last_X_s, last_X_ns) records when a periodic action last ran,
 * so we can compute when it should next fire without a separate timer. */
static unsigned int last_launch_s  = 0, last_launch_ns  = 0;
static unsigned int last_detect_s  = 0, last_detect_ns  = 0;
static unsigned int last_print_s   = 0, last_print_ns   = 0;

/* Real-time anchor used to enforce the WALL_CLOCK_LIMIT forking window
 * and the SIGALRM_LIMIT hard deadline. */
static time_t start_real;

/* Logging state. */
static FILE *logfp     = NULL;  /* log file handle (also written to stdout)  */
static int   log_lines = 0;    /* lines written so far; capped at MAX_LOG_LINES */

/* End-of-run statistics. */
static long stat_total_req  = 0;  /* total resource requests received         */
static long stat_imm_grants = 0;  /* requests granted immediately (not queued) */
static long stat_dl_runs    = 0;  /* number of deadlock detection passes       */
static long stat_dl_kills   = 0;  /* workers killed to resolve deadlocks       */


/* ════════════════════════════════════════════════════════════════════════════
   Logging
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * log_line — printf-style helper that writes to both stdout and the log file.
 *
 * Silently drops the message once log_lines reaches MAX_LOG_LINES, preventing
 * runaway log growth on very long or busy runs.  Each write is flushed
 * immediately so the file is readable even if oss is killed mid-run.
 */
static void log_line(const char *fmt, ...)
{
    if (log_lines >= MAX_LOG_LINES) return;

    va_list ap;

    /* Write to stdout. */
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);

    /* Write the same line to the log file. */
    if (logfp) {
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        fputc('\n', logfp);
        fflush(logfp);  /* flush immediately so content survives a SIGTERM */
        va_end(ap);
    }

    log_lines++;
}


/* ════════════════════════════════════════════════════════════════════════════
   Simulated-time helpers
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * sim_reached — return 1 if the sim clock has reached or passed (target_s, target_ns).
 *
 * Used to check whether enough simulated time has elapsed for a periodic
 * action (next launch, next detection run, next table print).
 */
static int sim_reached(unsigned int target_s, unsigned int target_ns)
{
    if (shm->clock.sec > target_s)  return 1;
    if (shm->clock.sec == target_s && shm->clock.ns >= target_ns) return 1;
    return 0;
}

/*
 * sim_add — add delta_ns nanoseconds to (base_s, base_ns), carrying into seconds.
 *
 * Writes the result into (*out_s, *out_ns).  Used to compute future deadlines
 * from the current sim-clock position.
 */
static void sim_add(unsigned int base_s,  unsigned int base_ns,
                    unsigned int delta_ns,
                    unsigned int *out_s,  unsigned int *out_ns)
{
    *out_ns = base_ns + delta_ns;
    *out_s  = base_s;
    if (*out_ns >= 1000000000u) {
        (*out_s)++;
        *out_ns -= 1000000000u;
    }
}


/* ════════════════════════════════════════════════════════════════════════════
   Wait-queue helpers
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * wq_enqueue — add slot to the tail of resource r's FIFO wait queue.
 *
 * Called when a request cannot be immediately satisfied.  The worker stays
 * blocked in msgrcv until oss dequeues it and sends a MSG_GRANT.
 */
static void wq_enqueue(int resource, int slot)
{
    wq[resource][wq_tail[resource]] = slot;
    wq_tail[resource] = (wq_tail[resource] + 1) % MAX_PROCESSES;
    wq_size[resource]++;
}

/*
 * wq_dequeue — remove and return the head slot from resource r's wait queue.
 *
 * The caller is responsible for actually granting the resource and
 * unblocking the dequeued process.
 */
static int wq_dequeue(int resource)
{
    int s = wq[resource][wq_head[resource]];
    wq_head[resource] = (wq_head[resource] + 1) % MAX_PROCESSES;
    wq_size[resource]--;
    return s;
}

/*
 * wq_remove_slot — remove a specific slot from every resource's wait queue.
 *
 * Called at the start of free_all_resources() before processing waiters.
 * This prevents a terminated process from being dequeued as a "waiter" and
 * receiving a resource grant after it has already been killed — which would
 * permanently leak one unit of that resource from the available pool.
 *
 * Each queue is rebuilt by copying non-matching entries into a temporary
 * array, then writing back; head resets to 0, tail to the new size.
 */
static void wq_remove_slot(int slot)
{
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        if (wq_size[r] == 0) continue;

        int tmp[MAX_PROCESSES];
        int n = 0;

        /* Collect all entries that are NOT the slot being removed. */
        for (int k = 0; k < wq_size[r]; k++) {
            int idx = (wq_head[r] + k) % MAX_PROCESSES;
            if (wq[r][idx] != slot)
                tmp[n++] = wq[r][idx];
        }

        /* Write the compacted queue back starting at index 0. */
        for (int k = 0; k < n; k++)
            wq[r][k] = tmp[k];

        wq_head[r] = 0;
        wq_tail[r] = n;
        wq_size[r] = n;
    }
}


/* ════════════════════════════════════════════════════════════════════════════
   Messaging helpers
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * send_to_worker — send a message to the worker in the given PCB slot.
 *
 * mtype is set to the worker's OS PID so the worker's msgrcv call (which
 * filters on its own PID) wakes up for exactly this message and no other.
 */
static void send_to_worker(int slot, int mvalue, int action)
{
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype      = (long)shm->proctable[slot].pid;
    msg.mvalue     = mvalue;
    msg.msg_action = action;
    if (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) == -1)
        perror("oss msgsnd to worker");
}


/* ════════════════════════════════════════════════════════════════════════════
   Resource management
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * free_all_resources — return every unit held by slot to the available pool
 *                      and satisfy as many queued waiters as possible.
 *
 * Must be called before clearing the PCB slot, whether on voluntary
 * termination (handle_terminate) or forced termination (deadlock resolution).
 *
 * IMPORTANT: wq_remove_slot() is called FIRST to strip the dying process
 * from all wait queues.  If this were done after the waiter loop, the victim
 * could be dequeued as a "waiter" for one of its own previously-held
 * resources, and oss would send it a MSG_GRANT — permanently losing one
 * unit from available[].
 */
static void free_all_resources(int slot)
{
    /* Strip the slot from all queues before processing waiters. */
    wq_remove_slot(slot);

    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        int held = shm->proctable[slot].allocation[r];
        if (held == 0) continue;

        /* Return held units to the available pool and clear the PCB record. */
        shm->available[r] += held;
        shm->proctable[slot].allocation[r] = 0;
        shm->request[slot][r] = 0;

        /* Wake as many queued waiters for this resource as inventory allows.
         * Each dequeued waiter gets one unit; the loop stops when either
         * the queue is empty or available runs out. */
        while (wq_size[r] > 0 && shm->available[r] > 0) {
            int waiter = wq_dequeue(r);
            shm->available[r]--;
            shm->proctable[waiter].allocation[r]++;
            shm->proctable[waiter].blocked          = 0;
            shm->proctable[waiter].blocked_resource = -1;
            shm->request[waiter][r]                 = 0;
            log_line("  Master granting P%d request R%d (from freed resources) at time %u:%u",
                     waiter, r, shm->clock.sec, shm->clock.ns);
            send_to_worker(waiter, r + 1, MSG_GRANT);
            turn_sent[waiter] = 1;  /* waiter is now unblocked and will reply */
        }
    }

    /* Clear the blocked flag in case it was set but the resource was in
     * a class where held == 0 (e.g., blocked but allocation not yet updated). */
    shm->proctable[slot].blocked          = 0;
    shm->proctable[slot].blocked_resource = -1;
}


/* ════════════════════════════════════════════════════════════════════════════
   Message handlers
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * handle_request — process a resource request from slot for resource.
 *
 * If the resource has available inventory, it is granted immediately and a
 * MSG_GRANT reply is sent (turn_sent stays 1 because the grant acts as the
 * next "turn" message the worker will wake to).
 *
 * If not available, the worker is enqueued and NO reply is sent; the worker
 * stays blocked in msgrcv until another process releases the resource and
 * free_all_resources (or handle_release) dequeues and grants it.
 */
static void handle_request(int slot, int resource)
{
    turn_sent[slot] = 0;  /* received worker's outgoing message; slot is now "free" */
    stat_total_req++;

    log_line("Master has detected Process P%d requesting R%d at time %u:%u",
             slot, resource, shm->clock.sec, shm->clock.ns);

    if (shm->available[resource] > 0) {
        /* Immediate grant. */
        shm->available[resource]--;
        shm->proctable[slot].allocation[resource]++;
        stat_imm_grants++;
        log_line("Master granting P%d request R%d at time %u:%u",
                 slot, resource, shm->clock.sec, shm->clock.ns);
        send_to_worker(slot, resource + 1, MSG_GRANT);
        turn_sent[slot] = 1;  /* worker will respond when it wakes from the grant */
    } else {
        /* Deferred: block the worker until someone frees this resource. */
        shm->proctable[slot].blocked          = 1;
        shm->proctable[slot].blocked_resource = resource;
        shm->request[slot][resource]          = 1;
        wq_enqueue(resource, slot);
        log_line("Master: P%d request R%d not granted, process blocked at time %u:%u",
                 slot, resource, shm->clock.sec, shm->clock.ns);
        /* turn_sent[slot] remains 0: the worker sent a message but we
         * sent no reply, so we are not "waiting" for another inbound from it.
         * dispatch_one_turn() skips blocked slots anyway. */
    }
}

/*
 * handle_release — process a resource release from slot for resource.
 *
 * Returns the unit to the available pool, then checks if the head of the
 * wait queue for that resource can now be satisfied.  Finally, sends a
 * MSG_TURN back to the releasing process so it continues its lifecycle.
 */
static void handle_release(int slot, int resource)
{
    turn_sent[slot] = 0;  /* received worker's outgoing message */

    log_line("Master has acknowledged Process P%d releasing R%d at time %u:%u",
             slot, resource, shm->clock.sec, shm->clock.ns);

    /* Return the unit to inventory. */
    shm->proctable[slot].allocation[resource]--;
    shm->available[resource]++;

    /* Grant the first waiter for this resource if one exists. */
    if (wq_size[resource] > 0 && shm->available[resource] > 0) {
        int waiter = wq_dequeue(resource);
        shm->available[resource]--;
        shm->proctable[waiter].allocation[resource]++;
        shm->proctable[waiter].blocked          = 0;
        shm->proctable[waiter].blocked_resource = -1;
        shm->request[waiter][resource]          = 0;
        log_line("Master granting P%d request R%d at time %u:%u",
                 waiter, resource, shm->clock.sec, shm->clock.ns);
        send_to_worker(waiter, resource + 1, MSG_GRANT);
        turn_sent[waiter] = 1;  /* waiter is now unblocked and will respond */
    }

    /* Give the releasing process its next turn. */
    send_to_worker(slot, 1, MSG_TURN);
    turn_sent[slot] = 1;
}

/*
 * handle_terminate — process a voluntary termination acknowledgement from slot.
 *
 * Called when a worker sends MSG_TERMINATE (either because its simulated time
 * limit expired, or in response to an oss-initiated MSG_TERMINATE during
 * deadlock resolution).
 *
 * The guard against occupied == 0 handles the race where the deadlock
 * resolver has already cleaned up the slot before the victim's ack arrives.
 */
static void handle_terminate(int slot)
{
    turn_sent[slot] = 0;  /* received worker's outgoing message */

    /* Guard: deadlock resolution may have already cleared this slot. */
    if (!shm->proctable[slot].occupied) return;

    pid_t pid = shm->proctable[slot].pid;

    /* Release all held resources before marking the slot free. */
    free_all_resources(slot);
    shm->proctable[slot].occupied = 0;
    shm->proctable[slot].pid      = 0;
    currently_running--;

    /* Block until the worker process has fully exited so we do not leak
     * a zombie.  The worker calls exit() immediately after sending its
     * terminate ack, so this returns almost instantly. */
    waitpid(pid, NULL, 0);

    log_line("Master: Process P%d has terminated at time %u:%u",
             slot, shm->clock.sec, shm->clock.ns);
}


/* ════════════════════════════════════════════════════════════════════════════
   Main-loop phases
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * reap_zombies — collect any worker exits that happened outside handle_terminate.
 *
 * Workers killed by signals (e.g., SIGTERM from sig_handler) exit without
 * sending a MSG_TERMINATE ack, so their slot accounting is done in sig_handler.
 * This call prevents zombie accumulation by reaping any that slipped through.
 */
static void reap_zombies(void)
{
    int status;
    /* WNOHANG: return immediately if no child has exited yet. */
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;  /* accounting is done in handle_terminate / sig_handler */
}

/*
 * find_free_slot — linear scan for the first unoccupied PCB slot.
 * Returns the slot index, or -1 if the table is full.
 */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!shm->proctable[i].occupied) return i;
    return -1;
}

/*
 * maybe_launch_worker — fork a new worker if all launch constraints are met.
 *
 * Guards (all must pass):
 *   1. total_launched < n_limit         — haven't reached the -n cap yet
 *   2. currently_running < s_limit      — haven't reached the -s cap
 *   3. currently_running < MAX_RUNNING  — haven't hit the hard process limit
 *   4. real elapsed < WALL_CLOCK_LIMIT  — still inside the 5-second fork window
 *   5. sim clock >= last launch + interval — respects the -i launch interval
 *
 * The child execs worker with five string arguments:
 *   slot  limitSec  limitNs  startClockSec  startClockNs
 *
 * startClockSec/Ns captures the sim time at fork so the worker can compute
 * its absolute deadline without extra communication.
 */
static void maybe_launch_worker(void)
{
    /* Guard 1–4: numeric limits and wall-clock window. */
    if (total_launched >= n_limit)                      return;
    if (currently_running >= s_limit)                   return;
    if (currently_running >= MAX_RUNNING)               return;
    if ((time(NULL) - start_real) >= WALL_CLOCK_LIMIT)  return;

    /* Guard 5: enforce simulated launch interval. */
    unsigned int next_s, next_ns;
    sim_add(last_launch_s, last_launch_ns, launch_int_ns, &next_s, &next_ns);
    if (!sim_reached(next_s, next_ns)) return;

    int slot = find_free_slot();
    if (slot == -1) return;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }

    if (pid == 0) {
        /* ── Child process ─────────────────────────────────────────────
         * Convert all arguments to strings and exec the worker binary.
         * The child inherits the open shared memory and message queue
         * file descriptors; worker.c re-attaches them by key. */
        char s_slot[16], s_tsec[16], s_tns[16], s_csec[16], s_cns[16];
        snprintf(s_slot, sizeof(s_slot), "%d", slot);
        snprintf(s_tsec, sizeof(s_tsec), "%d", t_limit_s);
        snprintf(s_tns,  sizeof(s_tns),  "%u", 0u);           /* no sub-second limit */
        snprintf(s_csec, sizeof(s_csec), "%u", shm->clock.sec);
        snprintf(s_cns,  sizeof(s_cns),  "%u", shm->clock.ns);
        execl("./worker", "worker", s_slot, s_tsec, s_tns, s_csec, s_cns, NULL);
        perror("execl");
        exit(1);
    }

    /* ── Parent: fill in the PCB for the new worker. ──────────────── */
    turn_sent[slot]                       = 0;  /* no message sent to slot yet */
    shm->proctable[slot].occupied         = 1;
    shm->proctable[slot].pid              = pid;
    shm->proctable[slot].blocked          = 0;
    shm->proctable[slot].blocked_resource = -1;
    memset(shm->proctable[slot].allocation, 0, sizeof(shm->proctable[slot].allocation));
    memset(shm->request[slot],              0, sizeof(shm->request[slot]));

    total_launched++;
    currently_running++;
    last_launch_s  = shm->clock.sec;
    last_launch_ns = shm->clock.ns;

    log_line("Master: forked worker P%d (pid %d) at time %u:%u",
             slot, (int)pid, shm->clock.sec, shm->clock.ns);
}

/*
 * process_messages — drain all pending worker messages with non-blocking reads.
 *
 * All worker→oss messages use mtype=1, so a single IPC_NOWAIT loop collects
 * everything that arrived since the last call.  Stops when msgrcv returns
 * ENOMSG (queue empty) and retries on EINTR (signal interrupted the syscall).
 *
 * Dispatches each message to the appropriate handler based on msg_action
 * and the sign of mvalue.
 */
static void process_messages(void)
{
    Message msg;
    memset(&msg, 0, sizeof(msg));

    while (1) {
        int rc = (int)msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), 1L, IPC_NOWAIT);
        if (rc == -1) {
            if (errno == ENOMSG) break;    /* queue is empty — done for this tick */
            if (errno == EINTR)  continue; /* interrupted by signal — retry        */
            perror("oss msgrcv");
            break;
        }

        int slot   = msg.sender_slot;
        int action = msg.msg_action;
        int val    = msg.mvalue;

        /* Sanity-check the slot index before touching the PCB. */
        if (slot < 0 || slot >= MAX_PROCESSES) continue;

        /* Route to the correct handler.
         * MSG_TERMINATE and mvalue==0 both signal voluntary exit. */
        if (action == MSG_TERMINATE || val == 0) {
            handle_terminate(slot);
        } else if (action == MSG_REQUEST && val > 0) {
            handle_request(slot, val - 1);   /* mvalue is 1-based */
        } else if (action == MSG_RELEASE && val < 0) {
            handle_release(slot, (-val) - 1); /* mvalue is −(1-based index) */
        }
    }
}

/*
 * dispatch_one_turn — send a MSG_TURN to the next eligible worker.
 *
 * Walks the process table in round-robin order starting from rr_index.
 * Skips slots that are: empty, blocked (waiting for a resource grant), or
 * already have a pending outbound message (turn_sent[i] == 1).
 *
 * Sends to exactly one worker per call, then returns.  This keeps the
 * scheduling fair and avoids flooding any single process.
 */
static void dispatch_one_turn(void)
{
    for (int checked = 0; checked < MAX_PROCESSES; checked++) {
        int i = rr_index % MAX_PROCESSES;
        rr_index++;

        if (!shm->proctable[i].occupied) continue;  /* slot empty            */
        if (shm->proctable[i].blocked)   continue;  /* waiting for a grant   */
        if (turn_sent[i])                continue;  /* already has a message  */

        send_to_worker(i, 1, MSG_TURN);
        turn_sent[i] = 1;
        return;
    }
}

/*
 * advance_clock — advance the simulated clock based on real elapsed time.
 *
 * Uses CLOCK_MONOTONIC to measure how much real time has passed since the
 * last call and adds the same amount to the simulated clock (1:1 ratio).
 * This makes the sim-clock speed independent of how fast the main loop
 * runs on a given machine, ensuring workers' 3-second simulated limits
 * always correspond to ~3 real seconds regardless of server load.
 *
 * A 50 ms cap prevents large jumps if the process was suspended
 * (e.g., during a slow fork/exec or scheduler preemption).
 */
static void advance_clock(void)
{
    static struct timespec prev;
    static int first = 1;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    /* On the first call, just record the baseline and return without
     * advancing (there is no meaningful "elapsed" yet). */
    if (first) { prev = now; first = 0; return; }

    long elapsed_ns = (long)(now.tv_sec  - prev.tv_sec)  * 1000000000L
                    + (long)(now.tv_nsec - prev.tv_nsec);
    prev = now;

    if (elapsed_ns <= 0) return;

    /* Cap: prevent a single huge jump from a scheduling pause. */
    if (elapsed_ns > 50000000L) elapsed_ns = 50000000L;  /* 50 ms max */

    /* Advance sim clock by the elapsed real nanoseconds (1:1 ratio). */
    shm->clock.ns += (unsigned int)elapsed_ns;
    if (shm->clock.ns >= 1000000000u) {
        shm->clock.sec++;
        shm->clock.ns -= 1000000000u;
    }
}


/* ════════════════════════════════════════════════════════════════════════════
   Deadlock detection  (Banker's algorithm — detection mode, not avoidance)
   ════════════════════════════════════════════════════════════════════════════ */

static void run_deadlock_detection(void);  /* forward declaration for recursion */

/*
 * run_deadlock_detection — identify and resolve deadlocked processes.
 *
 * Algorithm (resource-allocation graph reduction):
 *   1. Work[r]    = available[r]          (hypothetical free resources)
 *   2. Finish[i]  = !occupied[i]          (empty slots start as "done")
 *   3. Find i where Finish[i]==false and request[i][r] <= Work[r] for all r.
 *   4. If found: Work[r] += allocation[i][r]; Finish[i] = true; goto 3.
 *   5. Any remaining Finish[i]==false is deadlocked (cannot ever complete).
 *
 * Resolution: terminate one victim (the first in the deadlocked set), release
 * all its resources (which may unblock other waiters), then recurse to check
 * whether the deadlock set has been cleared.
 *
 * The victim_pid variable captures the PID before zeroing the PCB field so
 * waitpid() always has a valid argument.
 */
static void run_deadlock_detection(void)
{
    int Work[RESOURCE_CLASSES];
    int Finish[MAX_PROCESSES];

    /* Step 1 & 2: initialise Work and Finish vectors. */
    memcpy(Work, shm->available, sizeof(Work));
    for (int i = 0; i < MAX_PROCESSES; i++)
        Finish[i] = shm->proctable[i].occupied ? 0 : 1;

    /* Steps 3 & 4: iteratively mark processes that can run to completion.
     * A process can complete if every resource it is currently waiting for
     * exists in the Work (hypothetical available) set. */
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (Finish[i]) continue;

            /* Check whether all of this process's pending requests fit in Work. */
            int can_complete = 1;
            for (int r = 0; r < RESOURCE_CLASSES; r++) {
                if (shm->request[i][r] > Work[r]) { can_complete = 0; break; }
            }

            if (can_complete) {
                /* Hypothetically grant: add the process's holdings to Work
                 * as if it were to finish and release everything. */
                for (int r = 0; r < RESOURCE_CLASSES; r++)
                    Work[r] += shm->proctable[i].allocation[r];
                Finish[i] = 1;
                progress  = 1;  /* made progress; keep scanning */
            }
        }
    }

    /* Step 5: any Finish[i]==false at this point is in the deadlocked set. */
    int dead[MAX_PROCESSES];
    int ndead = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!Finish[i]) dead[ndead++] = i;

    if (ndead == 0) return;  /* no deadlock this pass */

    /* Log the full deadlocked set before choosing a victim. */
    char buf[512];
    int  off = 0;
    for (int k = 0; k < ndead; k++) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "P%d%s", dead[k], (k < ndead - 1) ? ", " : "");
    }
    log_line("Processes %s deadlocked", buf);
    log_line("Attempting to resolve deadlock...");

    /* Always kill the first deadlocked process (simple FIFO victim selection). */
    int victim = dead[0];

    /* Log which resources the victim is giving up. */
    off = 0;
    char rbuf[512] = {0};
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        if (shm->proctable[victim].allocation[r] > 0)
            off += snprintf(rbuf + off, sizeof(rbuf) - (size_t)off,
                            "R%d:%d ", r, shm->proctable[victim].allocation[r]);
    }
    log_line("Killing process P%d: Resources released: %s", victim, rbuf);

    /* Send terminate to the victim; it will send an ack (MSG_TERMINATE)
     * which handle_terminate() will receive and skip (occupied == 0 by then). */
    send_to_worker(victim, 0, MSG_TERMINATE);
    turn_sent[victim] = 1;  /* mark as "waiting for reply" to block further turns */

    /* Save PID before zeroing the PCB so waitpid() gets the correct value. */
    pid_t victim_pid = shm->proctable[victim].pid;

    /* Release all resources and dequeue the victim from any wait queues. */
    free_all_resources(victim);
    shm->proctable[victim].occupied = 0;
    shm->proctable[victim].pid      = 0;
    currently_running--;
    stat_dl_kills++;

    /* Non-blocking reap: the worker may not have exited yet; reap_zombies()
     * will collect it on a later main-loop iteration if it hasn't. */
    waitpid(victim_pid, NULL, WNOHANG);

    /* Recurse: releasing the victim's resources may have unblocked other
     * processes, potentially clearing the rest of the deadlock set. */
    run_deadlock_detection();
}

/*
 * maybe_run_deadlock — fire deadlock detection once per simulated second.
 *
 * Compares the current sim clock against last_detect_s.  The interval check
 * uses only the seconds field for simplicity; sub-second precision is not
 * required for this detection frequency.
 */
static void maybe_run_deadlock(void)
{
    if (shm->clock.sec < last_detect_s + (unsigned int)DETECT_INTERVAL_S) return;

    stat_dl_runs++;
    log_line("Master running deadlock detection at time %u:%u",
             shm->clock.sec, shm->clock.ns);

    run_deadlock_detection();

    /* Update the timestamp after running so the next check fires ~1 sim-second later. */
    last_detect_s  = shm->clock.sec;
    last_detect_ns = shm->clock.ns;
}


/* ════════════════════════════════════════════════════════════════════════════
   Periodic output
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * print_resource_table — dump the current resource allocation matrix.
 *
 * Format:
 *   OSS Resource Table at S:NNNNNNNNN
 *           R0  R1  R2  ...
 *   Avail:  N   N   N   ...
 *   P0:     N   N   N   ... [BLOCKED]
 *   P1:     ...
 *
 * Written to both stdout and logfp (without going through log_line so the
 * multi-line table is not artificially split across log_lines increments).
 * Still respects the MAX_LOG_LINES cap at the function entry point.
 */
static void print_resource_table(void)
{
    if (log_lines >= MAX_LOG_LINES) return;

    /* Table header with current sim time. */
    printf("\nOSS Resource Table at %u:%09u\n", shm->clock.sec, shm->clock.ns);
    if (logfp)
        fprintf(logfp, "\nOSS Resource Table at %u:%09u\n",
                shm->clock.sec, shm->clock.ns);

    /* Column headers: R0 R1 R2 ... */
    printf("        ");
    if (logfp) fprintf(logfp, "        ");
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        printf("R%-3d", r);
        if (logfp) fprintf(logfp, "R%-3d", r);
    }

    /* Available row. */
    printf("\nAvail:  ");
    if (logfp) fprintf(logfp, "\nAvail:  ");
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        printf("%-4d", shm->available[r]);
        if (logfp) fprintf(logfp, "%-4d", shm->available[r]);
    }
    printf("\n");
    if (logfp) fprintf(logfp, "\n");

    /* One row per occupied PCB slot showing that process's allocations. */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!shm->proctable[i].occupied) continue;

        printf("P%-7d", i);
        if (logfp) fprintf(logfp, "P%-7d", i);

        for (int r = 0; r < RESOURCE_CLASSES; r++) {
            printf("%-4d", shm->proctable[i].allocation[r]);
            if (logfp) fprintf(logfp, "%-4d", shm->proctable[i].allocation[r]);
        }

        /* Mark blocked processes so the table is easier to read. */
        const char *bstr = shm->proctable[i].blocked ? " [BLOCKED]" : "";
        printf("%s\n", bstr);
        if (logfp) fprintf(logfp, "%s\n", bstr);
    }

    printf("\n");
    if (logfp) fprintf(logfp, "\n");
}

/*
 * maybe_print_tables — print the resource table every 0.5 simulated seconds.
 *
 * Computes the next print deadline from the last print timestamp and checks
 * whether the sim clock has reached it.
 */
static void maybe_print_tables(void)
{
    unsigned int ts, tns;
    sim_add(last_print_s, last_print_ns, PRINT_INTERVAL_NS, &ts, &tns);
    if (!sim_reached(ts, tns)) return;

    print_resource_table();

    /* Advance the print baseline to the current sim time (not the deadline)
     * so the next interval is computed from when we actually printed. */
    last_print_s  = shm->clock.sec;
    last_print_ns = shm->clock.ns;
}


/* ════════════════════════════════════════════════════════════════════════════
   Statistics
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * print_statistics — emit the end-of-run summary to stdout and the log file.
 *
 * Called on both normal exit (end of main loop) and signal-driven exit
 * (sig_handler).  Computes the immediate-grant percentage only when there
 * were requests, to avoid division by zero.
 */
static void print_statistics(void)
{
    double pct = (stat_total_req > 0)
                 ? (100.0 * stat_imm_grants / stat_total_req) : 0.0;

    printf("\n=== OSS End-of-Run Statistics ===\n");
    printf("Total resource requests:           %ld\n", stat_total_req);
    printf("Immediately granted:               %ld (%.1f%%)\n", stat_imm_grants, pct);
    printf("Deadlock detection runs:           %ld\n", stat_dl_runs);
    printf("Processes terminated by deadlock:  %ld\n", stat_dl_kills);

    if (logfp) {
        fprintf(logfp, "\n=== OSS End-of-Run Statistics ===\n");
        fprintf(logfp, "Total resource requests:           %ld\n", stat_total_req);
        fprintf(logfp, "Immediately granted:               %ld (%.1f%%)\n",
                stat_imm_grants, pct);
        fprintf(logfp, "Deadlock detection runs:           %ld\n", stat_dl_runs);
        fprintf(logfp, "Processes terminated by deadlock:  %ld\n", stat_dl_kills);
    }
}


/* ════════════════════════════════════════════════════════════════════════════
   IPC setup / teardown
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * cleanup_ipc — detach and remove all IPC resources, then close the log file.
 *
 * Safe to call multiple times: each resource is NULLed / set to -1 after
 * removal so a second call is a no-op.  Called from both the normal exit
 * path (end of main) and all signal handlers (SIGINT, SIGTERM, SIGALRM).
 */
static void cleanup_ipc(void)
{
    if (shm)          shmdt(shm);             /* detach segment from address space  */
    if (shmid != -1)  shmctl(shmid, IPC_RMID, NULL); /* delete the segment          */
    if (msgid != -1)  msgctl(msgid, IPC_RMID, NULL); /* delete the message queue    */
    if (logfp)        fclose(logfp);           /* flush and close the log file       */

    /* Reset to sentinels so a second call is harmless. */
    shm   = NULL;
    shmid = -1;
    msgid = -1;
    logfp = NULL;
}

/*
 * sig_handler — unified handler for SIGINT, SIGTERM, and SIGALRM.
 *
 * On any of these signals:
 *   1. Send SIGTERM to every live worker so they exit (default action).
 *   2. Print end-of-run statistics.
 *   3. Remove all IPC resources and close the log.
 *   4. Exit 0 so the shell does not treat a SIGINT/SIGALRM as an error.
 *
 * SIGTERM is registered here so that the test harness's `timeout` command
 * (which delivers SIGTERM) triggers a clean shutdown instead of leaving
 * orphan worker processes and leaked IPC segments.
 */
static void sig_handler(int sig)
{
    (void)sig;  /* all three signals share this handler; signal identity is irrelevant */

    /* Kill all children. */
    if (shm) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (shm->proctable[i].occupied)
                kill(shm->proctable[i].pid, SIGTERM);
        }
    }

    print_statistics();
    cleanup_ipc();
    exit(0);
}

/*
 * setup_ipc — create and attach shared memory and message queue.
 *
 * Shared memory is zeroed with memset, then available[] is initialised to
 * INSTANCES_PER for each resource class (the starting state: nothing allocated).
 *
 * On failure, oss exits immediately.  partial cleanup is handled by the
 * init order: shmget before msgget, so if msgget fails, cleanup_ipc() can
 * still remove the segment.
 */
static void setup_ipc(void)
{
    /* Create (or attach to existing) shared memory segment. */
    shmid = shmget(SHM_KEY, sizeof(SharedMem), IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); exit(1); }

    shm = (SharedMem *)shmat(shmid, NULL, 0);  /* attach read-write */
    if (shm == (void *)-1) { perror("shmat"); exit(1); }

    /* Zero all fields, then set each resource to its full initial inventory. */
    memset(shm, 0, sizeof(SharedMem));
    for (int r = 0; r < RESOURCE_CLASSES; r++)
        shm->available[r] = INSTANCES_PER;

    /* Create the message queue. */
    msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msgid == -1) { perror("msgget"); cleanup_ipc(); exit(1); }

    /* Zero the wait queues (already zero from BSS, but explicit for clarity). */
    memset(wq,      0, sizeof(wq));
    memset(wq_head, 0, sizeof(wq_head));
    memset(wq_tail, 0, sizeof(wq_tail));
    memset(wq_size, 0, sizeof(wq_size));
}


/* ════════════════════════════════════════════════════════════════════════════
   main
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * usage — print a brief synopsis to stderr.
 */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-h] [-n proc] [-s simul] [-t timeLimitForChildren]\n"
            "          [-i launchIntervalMs] [-f logfile]\n",
            prog);
}

/*
 * main — parse options, initialise IPC and signals, run the scheduler loop.
 *
 * Main loop order each iteration:
 *   1. reap_zombies        — collect any silently-exited children
 *   2. maybe_launch_worker — fork next worker if constraints allow
 *   3. process_messages    — drain all pending worker messages
 *   4. dispatch_one_turn   — send a turn to the next eligible worker
 *   5. advance_clock       — update sim clock from real elapsed time
 *   6. maybe_run_deadlock  — run Banker's detection if 1 sim-second has passed
 *   7. maybe_print_tables  — print resource table if 0.5 sim-seconds have passed
 *
 * Exit condition: all intended workers have been launched AND currently_running
 * has dropped to zero (every worker has terminated).  The SIGALRM fires after
 * SIGALRM_LIMIT real seconds as an absolute safety valve.
 */
int main(int argc, char *argv[])
{
    /* Parse command-line options. */
    int opt;
    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
        case 'h': usage(argv[0]); return 0;
        case 'n': n_limit       = atoi(optarg);                             break;
        case 's': s_limit       = atoi(optarg);                             break;
        case 't': t_limit_s     = atoi(optarg);                             break;
        case 'i': launch_int_ns = (unsigned int)atoi(optarg) * 1000000u;   break;
        case 'f': snprintf(logfile, sizeof(logfile), "%s", optarg);         break;
        default:  usage(argv[0]); return 1;
        }
    }

    /* Clamp the simultaneous-worker limit to the hard cap. */
    if (s_limit > MAX_RUNNING) s_limit = MAX_RUNNING;

    /* Open the log file before IPC setup so the "OSS started" line is the first entry. */
    logfp = fopen(logfile, "w");
    if (!logfp) { perror("fopen logfile"); return 1; }

    setup_ipc();

    /* Register signal handlers.  SIGTERM is handled so that the test harness's
     * `timeout` command (which delivers SIGTERM) triggers a clean shutdown. */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGALRM, sig_handler);
    alarm(SIGALRM_LIMIT);       /* absolute wall-clock deadline */
    start_real = time(NULL);    /* anchor for WALL_CLOCK_LIMIT and alarm checks */

    log_line("OSS started: n=%d s=%d t=%d i=%ums logfile=%s",
             n_limit, s_limit, t_limit_s, launch_int_ns / 1000000u, logfile);

    /* ── Scheduler loop ──────────────────────────────────────────────── */
    int done = 0;
    while (!done) {
        reap_zombies();
        maybe_launch_worker();
        process_messages();
        dispatch_one_turn();
        advance_clock();
        maybe_run_deadlock();
        maybe_print_tables();

        /* Check termination: stop forking once we've launched enough workers
         * or the wall-clock window has expired; exit once everyone has gone. */
        int stop_forking = (total_launched >= n_limit)
                        || ((time(NULL) - start_real) >= WALL_CLOCK_LIMIT);
        done = stop_forking && (currently_running == 0);
    }

    print_statistics();
    cleanup_ipc();
    return 0;
}
