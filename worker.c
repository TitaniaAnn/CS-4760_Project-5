#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include "shared.h"

static SharedMem *shm      = NULL;
static int        shmid    = -1;
static int        msgid    = -1;
static int        my_slot  = -1;
static int        my_alloc[RESOURCE_CLASSES];

/* ── Simulated-time comparison helpers ─────────────────────────── */

static int sim_exceeded(unsigned int start_s, unsigned int start_ns,
                        unsigned int lim_s,   unsigned int lim_ns)
{
    /* deadline = start + limit */
    unsigned int dead_s  = start_s + lim_s;
    unsigned int dead_ns = start_ns + lim_ns;
    if (dead_ns >= 1000000000u) { dead_s++; dead_ns -= 1000000000u; }

    unsigned int cur_s  = shm->clock.sec;
    unsigned int cur_ns = shm->clock.ns;

    if (cur_s > dead_s)  return 1;
    if (cur_s == dead_s && cur_ns >= dead_ns) return 1;
    return 0;
}

/* ── Resource helpers ───────────────────────────────────────────── */

/* Return a random resource index we can still request (alloc < INSTANCES_PER).
   Returns -1 if every resource is already at max allocation. */
static int pick_requestable(void)
{
    int candidates[RESOURCE_CLASSES];
    int n = 0;
    for (int r = 0; r < RESOURCE_CLASSES; r++)
        if (my_alloc[r] < INSTANCES_PER)
            candidates[n++] = r;
    if (n == 0) return -1;
    return candidates[rand() % n];
}

/* Return a random resource index we currently hold at least one unit of.
   Returns -1 if we hold nothing. */
static int pick_releasable(void)
{
    int candidates[RESOURCE_CLASSES];
    int n = 0;
    for (int r = 0; r < RESOURCE_CLASSES; r++)
        if (my_alloc[r] > 0)
            candidates[n++] = r;
    if (n == 0) return -1;
    return candidates[rand() % n];
}

/* ── Messaging helpers ──────────────────────────────────────────── */

static void send_to_oss(int mvalue, int action)
{
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype       = 1;          /* all worker→oss messages use mtype=1 */
    msg.mvalue      = mvalue;
    msg.sender_slot = my_slot;
    msg.msg_action  = action;
    if (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
        perror("worker msgsnd");
        exit(1);
    }
}

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

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc != 6) {
        fprintf(stderr, "Usage: worker slot limitSec limitNs startSec startNs\n");
        exit(1);
    }

    my_slot = atoi(argv[1]);
    unsigned int lim_s  = (unsigned int)atoi(argv[2]);
    unsigned int lim_ns = (unsigned int)atoi(argv[3]);
    unsigned int sta_s  = (unsigned int)atoi(argv[4]);
    unsigned int sta_ns = (unsigned int)atoi(argv[5]);

    shmid = shmget(SHM_KEY, sizeof(SharedMem), 0666);
    if (shmid == -1) { perror("worker shmget"); exit(1); }
    shm = (SharedMem *)shmat(shmid, NULL, SHM_RDONLY);
    if (shm == (void *)-1) { perror("worker shmat"); exit(1); }

    msgid = msgget(MSG_KEY, 0666);
    if (msgid == -1) { perror("worker msgget"); exit(1); }

    srand((unsigned int)getpid());
    memset(my_alloc, 0, sizeof(my_alloc));

    while (1) {
        Message msg = recv_from_oss();

        /* oss told us to terminate */
        if (msg.msg_action == MSG_TERMINATE) {
            send_to_oss(0, MSG_TERMINATE);
            shmdt(shm);
            exit(0);
        }

        /* if this was a grant, record the newly allocated resource */
        if (msg.msg_action == MSG_GRANT && msg.mvalue > 0) {
            my_alloc[msg.mvalue - 1]++;
        }

        /* check our simulated time limit */
        if (sim_exceeded(sta_s, sta_ns, lim_s, lim_ns)) {
            send_to_oss(0, MSG_TERMINATE);
            shmdt(shm);
            exit(0);
        }

        /* decide: 70% request, 30% release */
        if (rand() % 100 < 70) {
            int r = pick_requestable();
            if (r == -1) r = pick_releasable();
            if (r == -1) continue;   /* nothing to do this turn */
            /* send request; then go back to top and block in recv for grant */
            send_to_oss(r + 1, MSG_REQUEST);
        } else {
            int r = pick_releasable();
            if (r == -1) r = pick_requestable();
            if (r == -1) continue;
            my_alloc[r]--;
            send_to_oss(-(r + 1), MSG_RELEASE);
            /* oss will send us a MSG_TURN so we loop back naturally */
        }
    }

    shmdt(shm);
    return 0;
}
