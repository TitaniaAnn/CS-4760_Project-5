/*
 * worker.c — Child resource-consumer process
 *
 * CS-4760 Project 5
 * Author: Cynthia Sims
 *
 * Each worker is forked and exec'd by oss with five arguments:
 *   argv[1] = slot        — index into oss's PCB table
 *   argv[2] = limitSec    — simulated time limit (whole seconds)
 *   argv[3] = limitNs     — simulated time limit (nanosecond fraction)
 *   argv[4] = startSec    — sim-clock value at time of fork (seconds)
 *   argv[5] = startNs     — sim-clock value at time of fork (ns fraction)
 *
 * The worker attaches to shared memory read-only (it only reads the
 * simulated clock to check its time limit) and to the message queue
 * for bidirectional communication with oss.
 *
 * Main loop:
 *   1. Block in msgrcv waiting for a message addressed to this PID.
 *   2. If MSG_TERMINATE: acknowledge and exit.
 *   3. If MSG_GRANT: record the newly allocated resource in local state.
 *   4. Check the simulated time limit; if exceeded, send terminate and exit.
 *   5. Pick an action: 70% chance request one resource, 30% chance release one.
 *      Fallback to the opposite action if the preferred one is not possible.
 *   6. Send the request or release to oss, then go back to step 1.
 *
 * After a request: oss either grants immediately (MSG_GRANT arrives) or
 * blocks the worker until a resource is freed (MSG_GRANT arrives later).
 * Either way, the worker wakes at the top of the loop through the grant.
 *
 * After a release: oss sends a MSG_TURN so the worker continues.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include "shared.h"

/* IPC handles and process-table slot, set once at startup. */
static SharedMem *shm     = NULL;
static int        shmid   = -1;
static int        msgid   = -1;
static int        my_slot = -1;

/* Local allocation tracking — mirrors oss's authoritative record but is
 * maintained independently so the worker knows which resources it can release.
 * oss is the ground truth; this is used only to drive pick_requestable /
 * pick_releasable decisions within the worker. */
static int my_alloc[RESOURCE_CLASSES];


/* ════════════════════════════════════════════════════════════════════════════
   Simulated-time comparison
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * sim_exceeded — return 1 if the current sim clock has passed the deadline.
 *
 * The deadline is computed as (start_s + lim_s, start_ns + lim_ns), with a
 * carry if the nanosecond sum overflows 1 billion.
 *
 * Workers read the clock from shared memory (read-only attach); only oss
 * writes it.  The check is performed each time the worker receives a message
 * so a blocked worker (one waiting in msgrcv for a grant) will notice the
 * expiry as soon as it wakes.
 */
static int sim_exceeded(unsigned int start_s, unsigned int start_ns,
                        unsigned int lim_s,   unsigned int lim_ns)
{
    /* Compute absolute deadline with carry. */
    unsigned int dead_s  = start_s + lim_s;
    unsigned int dead_ns = start_ns + lim_ns;
    if (dead_ns >= 1000000000u) {
        dead_s++;
        dead_ns -= 1000000000u;
    }

    unsigned int cur_s  = shm->clock.sec;
    unsigned int cur_ns = shm->clock.ns;

    /* Compare: current time >= deadline? */
    if (cur_s > dead_s)  return 1;
    if (cur_s == dead_s && cur_ns >= dead_ns) return 1;
    return 0;
}


/* ════════════════════════════════════════════════════════════════════════════
   Resource selection helpers
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * pick_requestable — choose a random resource class that can still be requested.
 *
 * A resource is requestable if the worker holds fewer than INSTANCES_PER units
 * of that class (the per-process allocation cap).  Builds the candidate set
 * and selects uniformly at random.
 *
 * Returns the resource index, or -1 if every class is already at its per-
 * process maximum (nothing can be requested this turn).
 */
static int pick_requestable(void)
{
    int candidates[RESOURCE_CLASSES];
    int n = 0;
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        if (my_alloc[r] < INSTANCES_PER)
            candidates[n++] = r;
    }
    if (n == 0) return -1;
    return candidates[rand() % n];
}

/*
 * pick_releasable — choose a random resource class that the worker currently holds.
 *
 * A resource is releasable if the worker holds at least one unit.  Builds
 * the candidate set and selects uniformly at random.
 *
 * Returns the resource index, or -1 if the worker holds nothing (nothing to
 * release this turn).
 */
static int pick_releasable(void)
{
    int candidates[RESOURCE_CLASSES];
    int n = 0;
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        if (my_alloc[r] > 0)
            candidates[n++] = r;
    }
    if (n == 0) return -1;
    return candidates[rand() % n];
}


/* ════════════════════════════════════════════════════════════════════════════
   Messaging helpers
   ════════════════════════════════════════════════════════════════════════════ */

/*
 * send_to_oss — send a message to oss.
 *
 * All worker→oss messages use mtype=1 so oss can drain them with a single
 * IPC_NOWAIT loop without knowing individual worker PIDs.
 *
 * sender_slot is always filled so oss can look up the PCB entry without a
 * linear PID-to-slot scan.
 */
static void send_to_oss(int mvalue, int action)
{
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype       = 1;           /* all worker→oss messages share this type */
    msg.mvalue      = mvalue;
    msg.sender_slot = my_slot;
    msg.msg_action  = action;
    if (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
        perror("worker msgsnd");
        exit(1);
    }
}

/*
 * recv_from_oss — block until oss sends a message addressed to this PID.
 *
 * Uses the worker's OS PID as the mtype filter so only messages intended
 * for this worker (MSG_TURN, MSG_GRANT, MSG_TERMINATE from oss) wake it.
 * Messages sent to other workers' PIDs pass through the same queue
 * without disturbing this call.
 */
static Message recv_from_oss(void)
{
    Message msg;
    memset(&msg, 0, sizeof(msg));
    if (msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), (long)getpid(), 0) == -1) {
        perror("worker msgrcv");
        exit(1);
    }
    return msg;
}


/* ════════════════════════════════════════════════════════════════════════════
   main
   ════════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    /* Validate argument count; oss always passes exactly 5 positional args. */
    if (argc != 6) {
        fprintf(stderr, "Usage: worker slot limitSec limitNs startSec startNs\n");
        exit(1);
    }

    /* Parse arguments passed by oss at exec time. */
    my_slot              = atoi(argv[1]);
    unsigned int lim_s   = (unsigned int)atoi(argv[2]);  /* simulated time limit  */
    unsigned int lim_ns  = (unsigned int)atoi(argv[3]);
    unsigned int sta_s   = (unsigned int)atoi(argv[4]);  /* sim-clock at fork     */
    unsigned int sta_ns  = (unsigned int)atoi(argv[5]);

    /* Attach shared memory read-only — the worker only reads the sim clock.
     * All resource-table writes are done exclusively by oss. */
    shmid = shmget(SHM_KEY, sizeof(SharedMem), 0666);
    if (shmid == -1) { perror("worker shmget"); exit(1); }
    shm = (SharedMem *)shmat(shmid, NULL, SHM_RDONLY);
    if (shm == (void *)-1) { perror("worker shmat"); exit(1); }

    /* Connect to the existing message queue (no IPC_CREAT — oss owns it). */
    msgid = msgget(MSG_KEY, 0666);
    if (msgid == -1) { perror("worker msgget"); exit(1); }

    /* Seed with PID so each worker has an independent random sequence. */
    srand((unsigned int)getpid());
    memset(my_alloc, 0, sizeof(my_alloc));

    /* ── Main loop ────────────────────────────────────────────────────
     * Block on msgrcv each iteration; oss controls when we run by sending
     * MSG_TURN or MSG_GRANT.  We never busy-spin. */
    while (1) {
        Message msg = recv_from_oss();

        /* ── Terminate order from oss (deadlock resolution) ──────── */
        if (msg.msg_action == MSG_TERMINATE) {
            /* Acknowledge the termination so oss's handle_terminate
             * (invoked if slot is still occupied) can clean up properly. */
            send_to_oss(0, MSG_TERMINATE);
            shmdt(shm);
            exit(0);
        }

        /* ── Grant: update local allocation record ────────────────── */
        /* mvalue is 1-based; resource index is mvalue-1.
         * Only update on MSG_GRANT with a positive mvalue (not a bare turn). */
        if (msg.msg_action == MSG_GRANT && msg.mvalue > 0) {
            my_alloc[msg.mvalue - 1]++;
        }

        /* ── Simulated time limit check ───────────────────────────── */
        /* Checked on every wakeup (turn or grant) so a blocked worker
         * notices expiry as soon as it is woken by the grant. */
        if (sim_exceeded(sta_s, sta_ns, lim_s, lim_ns)) {
            send_to_oss(0, MSG_TERMINATE);
            shmdt(shm);
            exit(0);
        }

        /* ── Decide: 70% request, 30% release ────────────────────── */
        /*
         * Preferred action is attempted first; if it is not possible (e.g.,
         * nothing to release, or all classes at their per-process cap),
         * the opposite action is tried.  If neither is possible (holding
         * nothing AND at max on everything), the loop continues without
         * sending a message — oss will send another turn next round.
         */
        if (rand() % 100 < 70) {
            /* Preferred: request a resource. */
            int r = pick_requestable();
            if (r != -1) {
                /* Send request; we will block in recv_from_oss until oss
                 * sends a MSG_GRANT (immediately or after a waiter dequeue). */
                send_to_oss(r + 1, MSG_REQUEST);
            } else {
                /* Fallback: nothing requestable; release instead. */
                r = pick_releasable();
                if (r == -1) continue;  /* nothing to do this turn */
                my_alloc[r]--;
                send_to_oss(-(r + 1), MSG_RELEASE);
            }
        } else {
            /* Preferred: release a resource. */
            int r = pick_releasable();
            if (r != -1) {
                my_alloc[r]--;
                send_to_oss(-(r + 1), MSG_RELEASE);
            } else {
                /* Fallback: nothing to release; request instead. */
                r = pick_requestable();
                if (r == -1) continue;  /* nothing to do this turn */
                send_to_oss(r + 1, MSG_REQUEST);
            }
        }
        /* After sending: go back to recv_from_oss to wait for the reply.
         * For requests: oss sends MSG_GRANT (blocking or immediate).
         * For releases: oss sends MSG_TURN after updating availability. */
    }
}
