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

/* ════════════════════════════════════════════════════════════════
   Global state
   ════════════════════════════════════════════════════════════════ */

static SharedMem *shm   = NULL;
static int        shmid = -1;
static int        msgid = -1;

/* Per-resource wait queues (oss-private, not in shared memory) */
static int wq[RESOURCE_CLASSES][MAX_PROCESSES];
static int wq_head[RESOURCE_CLASSES];
static int wq_tail[RESOURCE_CLASSES];
static int wq_size[RESOURCE_CLASSES];

/* Round-robin dispatch */
static int rr_index = 0;

/* Process accounting */
static int total_launched    = 0;
static int currently_running = 0;

/* Config (command-line) */
static int          n_limit        = 20;      /* -n */
static int          s_limit        = 5;       /* -s */
static int          t_limit_s      = 3;       /* -t child time limit (seconds) */
static unsigned int launch_int_ns  = 100000000u; /* -i (nanoseconds, default 100ms) */
static char         logfile[256]   = "oss.log";

/* Timing */
static unsigned int last_launch_s  = 0, last_launch_ns  = 0;
static unsigned int last_detect_s  = 0, last_detect_ns  = 0;
static unsigned int last_print_s   = 0, last_print_ns   = 0;
static time_t       start_real;

/* Logging */
static FILE *logfp     = NULL;
static int   log_lines = 0;

/* Statistics */
static long stat_total_req   = 0;
static long stat_imm_grants  = 0;
static long stat_dl_runs     = 0;
static long stat_dl_kills    = 0;

/* ════════════════════════════════════════════════════════════════
   Logging
   ════════════════════════════════════════════════════════════════ */

static void log_line(const char *fmt, ...)
{
    if (log_lines >= MAX_LOG_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    va_end(ap);

    if (logfp) {
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        fputc('\n', logfp);
        va_end(ap);
    }
    log_lines++;
}

/* ════════════════════════════════════════════════════════════════
   Simulated-time helpers
   ════════════════════════════════════════════════════════════════ */

/* Return 1 if current sim clock >= (target_s, target_ns) */
static int sim_reached(unsigned int target_s, unsigned int target_ns)
{
    if (shm->clock.sec > target_s)  return 1;
    if (shm->clock.sec == target_s && shm->clock.ns >= target_ns) return 1;
    return 0;
}

/* Add nanoseconds to a (s, ns) pair, returning the result. */
static void sim_add(unsigned int base_s,  unsigned int base_ns,
                    unsigned int delta_ns,
                    unsigned int *out_s,  unsigned int *out_ns)
{
    *out_ns = base_ns + delta_ns;
    *out_s  = base_s;
    if (*out_ns >= 1000000000u) { (*out_s)++; *out_ns -= 1000000000u; }
}

/* ════════════════════════════════════════════════════════════════
   Wait-queue helpers
   ════════════════════════════════════════════════════════════════ */

static void wq_enqueue(int resource, int slot)
{
    wq[resource][wq_tail[resource]] = slot;
    wq_tail[resource] = (wq_tail[resource] + 1) % MAX_PROCESSES;
    wq_size[resource]++;
}

static int wq_dequeue(int resource)
{
    int s = wq[resource][wq_head[resource]];
    wq_head[resource] = (wq_head[resource] + 1) % MAX_PROCESSES;
    wq_size[resource]--;
    return s;
}

/* Remove a specific slot from all wait queues (e.g., on termination). */
static void wq_remove_slot(int slot)
{
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        if (wq_size[r] == 0) continue;
        int tmp[MAX_PROCESSES];
        int n = 0;
        for (int k = 0; k < wq_size[r]; k++) {
            int idx = (wq_head[r] + k) % MAX_PROCESSES;
            if (wq[r][idx] != slot)
                tmp[n++] = wq[r][idx];
        }
        for (int k = 0; k < n; k++) wq[r][k] = tmp[k];
        wq_head[r] = 0;
        wq_tail[r] = n;
        wq_size[r] = n;
    }
}

/* ════════════════════════════════════════════════════════════════
   Messaging helpers
   ════════════════════════════════════════════════════════════════ */

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

/* ════════════════════════════════════════════════════════════════
   Resource management
   ════════════════════════════════════════════════════════════════ */

/* Release all resources held by slot, granting queued waiters as we go. */
static void free_all_resources(int slot)
{
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        int held = shm->proctable[slot].allocation[r];
        if (held == 0) continue;
        shm->available[r] += held;
        shm->proctable[slot].allocation[r] = 0;
        shm->request[slot][r] = 0;

        /* satisfy one waiter per freed unit if possible */
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
        }
    }
    shm->proctable[slot].blocked          = 0;
    shm->proctable[slot].blocked_resource = -1;
    wq_remove_slot(slot);
}

/* ════════════════════════════════════════════════════════════════
   Message handlers
   ════════════════════════════════════════════════════════════════ */

static void handle_request(int slot, int resource)
{
    stat_total_req++;
    log_line("Master has detected Process P%d requesting R%d at time %u:%u",
             slot, resource, shm->clock.sec, shm->clock.ns);

    if (shm->available[resource] > 0) {
        shm->available[resource]--;
        shm->proctable[slot].allocation[resource]++;
        stat_imm_grants++;
        log_line("Master granting P%d request R%d at time %u:%u",
                 slot, resource, shm->clock.sec, shm->clock.ns);
        send_to_worker(slot, resource + 1, MSG_GRANT);
    } else {
        /* block — no reply; worker stays in msgrcv */
        shm->proctable[slot].blocked          = 1;
        shm->proctable[slot].blocked_resource = resource;
        shm->request[slot][resource]          = 1;
        wq_enqueue(resource, slot);
        log_line("Master: P%d request R%d not granted, process blocked at time %u:%u",
                 slot, resource, shm->clock.sec, shm->clock.ns);
    }
}

static void handle_release(int slot, int resource)
{
    log_line("Master has acknowledged Process P%d releasing R%d at time %u:%u",
             slot, resource, shm->clock.sec, shm->clock.ns);
    shm->proctable[slot].allocation[resource]--;
    shm->available[resource]++;

    /* grant first waiter for this resource if possible */
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
    }

    /* give the releasing process its next turn */
    send_to_worker(slot, 1, MSG_TURN);
}

static void handle_terminate(int slot)
{
    /* Guard: if already cleaned up (e.g., deadlock victim), skip. */
    if (!shm->proctable[slot].occupied) return;

    pid_t pid = shm->proctable[slot].pid;
    free_all_resources(slot);
    shm->proctable[slot].occupied = 0;
    shm->proctable[slot].pid      = 0;
    currently_running--;

    /* Reap child */
    waitpid(pid, NULL, 0);
    log_line("Master: Process P%d has terminated at time %u:%u",
             slot, shm->clock.sec, shm->clock.ns);
}

/* ════════════════════════════════════════════════════════════════
   Main-loop phases
   ════════════════════════════════════════════════════════════════ */

static void reap_zombies(void)
{
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;  /* actual accounting done in handle_terminate */
}

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!shm->proctable[i].occupied) return i;
    return -1;
}

static void maybe_launch_worker(void)
{
    if (total_launched >= n_limit)                        return;
    if (currently_running >= s_limit)                     return;
    if (currently_running >= MAX_RUNNING)                 return;
    if ((time(NULL) - start_real) >= WALL_CLOCK_LIMIT)    return;

    /* enforce simulated launch interval */
    unsigned int next_s, next_ns;
    sim_add(last_launch_s, last_launch_ns, launch_int_ns, &next_s, &next_ns);
    if (!sim_reached(next_s, next_ns)) return;

    int slot = find_free_slot();
    if (slot == -1) return;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }

    if (pid == 0) {
        /* child */
        char s_slot[16], s_tsec[16], s_tns[16], s_csec[16], s_cns[16];
        snprintf(s_slot, sizeof(s_slot), "%d", slot);
        snprintf(s_tsec, sizeof(s_tsec), "%d", t_limit_s);
        snprintf(s_tns,  sizeof(s_tns),  "%u", 0u);
        snprintf(s_csec, sizeof(s_csec), "%u", shm->clock.sec);
        snprintf(s_cns,  sizeof(s_cns),  "%u", shm->clock.ns);
        execl("./worker", "worker", s_slot, s_tsec, s_tns, s_csec, s_cns, NULL);
        perror("execl");
        exit(1);
    }

    /* parent */
    shm->proctable[slot].occupied         = 1;
    shm->proctable[slot].pid              = pid;
    shm->proctable[slot].blocked          = 0;
    shm->proctable[slot].blocked_resource = -1;
    memset(shm->proctable[slot].allocation, 0,
           sizeof(shm->proctable[slot].allocation));
    memset(shm->request[slot], 0, sizeof(shm->request[slot]));

    total_launched++;
    currently_running++;
    last_launch_s  = shm->clock.sec;
    last_launch_ns = shm->clock.ns;

    log_line("Master: forked worker P%d (pid %d) at time %u:%u",
             slot, (int)pid, shm->clock.sec, shm->clock.ns);
}

static void process_messages(void)
{
    Message msg;
    memset(&msg, 0, sizeof(msg));

    /* drain all pending messages with non-blocking recv */
    while (1) {
        int rc = (int)msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), 1L, IPC_NOWAIT);
        if (rc == -1) {
            if (errno == ENOMSG) break;
            if (errno == EINTR)  continue;
            perror("oss msgrcv");
            break;
        }

        int slot   = msg.sender_slot;
        int action = msg.msg_action;
        int val    = msg.mvalue;

        if (slot < 0 || slot >= MAX_PROCESSES) continue;

        if (action == MSG_TERMINATE || val == 0) {
            handle_terminate(slot);
        } else if (action == MSG_REQUEST && val > 0) {
            handle_request(slot, val - 1);
        } else if (action == MSG_RELEASE && val < 0) {
            handle_release(slot, (-val) - 1);
        }
    }
}

static void dispatch_one_turn(void)
{
    for (int checked = 0; checked < MAX_PROCESSES; checked++) {
        int i = rr_index % MAX_PROCESSES;
        rr_index++;
        if (!shm->proctable[i].occupied) continue;
        if (shm->proctable[i].blocked)   continue;
        send_to_worker(i, 1, MSG_TURN);
        return;
    }
}

static void advance_clock(void)
{
    shm->clock.ns += CLOCK_INC_NS;
    if (shm->clock.ns >= 1000000000u) {
        shm->clock.sec++;
        shm->clock.ns -= 1000000000u;
    }
}

/* ════════════════════════════════════════════════════════════════
   Deadlock detection (Banker's algorithm, detection mode)
   ════════════════════════════════════════════════════════════════ */

static void run_deadlock_detection(void);   /* forward decl for recursion */

static void run_deadlock_detection(void)
{
    int Work[RESOURCE_CLASSES];
    int Finish[MAX_PROCESSES];

    memcpy(Work, shm->available, sizeof(Work));
    for (int i = 0; i < MAX_PROCESSES; i++)
        Finish[i] = shm->proctable[i].occupied ? 0 : 1;

    /* iteratively mark processes that can run to completion */
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (Finish[i]) continue;
            int ok = 1;
            for (int r = 0; r < RESOURCE_CLASSES; r++) {
                if (shm->request[i][r] > Work[r]) { ok = 0; break; }
            }
            if (ok) {
                for (int r = 0; r < RESOURCE_CLASSES; r++)
                    Work[r] += shm->proctable[i].allocation[r];
                Finish[i] = 1;
                progress  = 1;
            }
        }
    }

    /* collect deadlocked set */
    int dead[MAX_PROCESSES];
    int ndead = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!Finish[i]) dead[ndead++] = i;

    if (ndead == 0) return;

    /* log the deadlocked processes */
    char buf[512];
    int  off = 0;
    for (int k = 0; k < ndead; k++) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "P%d%s", dead[k], (k < ndead - 1) ? ", " : "");
    }
    log_line("Processes %s deadlocked", buf);
    log_line("Attempting to resolve deadlock...");

    /* pick the first victim */
    int victim = dead[0];

    /* log resources being released */
    off = 0;
    char rbuf[512] = {0};
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        if (shm->proctable[victim].allocation[r] > 0)
            off += snprintf(rbuf + off, sizeof(rbuf) - (size_t)off,
                            "R%d:%d ", r, shm->proctable[victim].allocation[r]);
    }
    log_line("Killing process P%d: Resources released: %s", victim, rbuf);

    /* send terminate, free resources, mark slot free */
    send_to_worker(victim, 0, MSG_TERMINATE);
    pid_t victim_pid = shm->proctable[victim].pid;
    free_all_resources(victim);
    shm->proctable[victim].occupied = 0;
    shm->proctable[victim].pid      = 0;
    currently_running--;
    stat_dl_kills++;

    /* reap — worker may take a moment to exit; reaped later if not yet done */
    waitpid(victim_pid, NULL, WNOHANG);

    /* recurse to check if deadlock persists */
    run_deadlock_detection();
}

static void maybe_run_deadlock(void)
{
    unsigned int ts, tns;
    sim_add(last_detect_s, last_detect_ns,
            (unsigned int)DETECT_INTERVAL_S * 1000000000u,
            &ts, &tns);
    /* Use seconds directly for a 1-second interval */
    if (shm->clock.sec < last_detect_s + (unsigned int)DETECT_INTERVAL_S) return;

    stat_dl_runs++;
    log_line("Master running deadlock detection at time %u:%u",
             shm->clock.sec, shm->clock.ns);
    run_deadlock_detection();

    last_detect_s  = shm->clock.sec;
    last_detect_ns = shm->clock.ns;
    (void)ts; (void)tns;   /* suppress unused-variable warning */
}

/* ════════════════════════════════════════════════════════════════
   Periodic output
   ════════════════════════════════════════════════════════════════ */

static void print_resource_table(void)
{
    printf("\nOSS Resource Table at %u:%09u\n", shm->clock.sec, shm->clock.ns);
    if (logfp)
        fprintf(logfp, "\nOSS Resource Table at %u:%09u\n",
                shm->clock.sec, shm->clock.ns);

    /* header */
    printf("        ");
    if (logfp) fprintf(logfp, "        ");
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        printf("R%-3d", r);
        if (logfp) fprintf(logfp, "R%-3d", r);
    }
    printf("\nAvail:  ");
    if (logfp) fprintf(logfp, "\nAvail:  ");
    for (int r = 0; r < RESOURCE_CLASSES; r++) {
        printf("%-4d", shm->available[r]);
        if (logfp) fprintf(logfp, "%-4d", shm->available[r]);
    }
    printf("\n");
    if (logfp) fprintf(logfp, "\n");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!shm->proctable[i].occupied) continue;
        printf("P%-7d", i);
        if (logfp) fprintf(logfp, "P%-7d", i);
        for (int r = 0; r < RESOURCE_CLASSES; r++) {
            printf("%-4d", shm->proctable[i].allocation[r]);
            if (logfp) fprintf(logfp, "%-4d", shm->proctable[i].allocation[r]);
        }
        const char *bstr = shm->proctable[i].blocked ? " [BLOCKED]" : "";
        printf("%s\n", bstr);
        if (logfp) fprintf(logfp, "%s\n", bstr);
    }
    printf("\n");
    if (logfp) fprintf(logfp, "\n");
}

static void maybe_print_tables(void)
{
    unsigned int ts, tns;
    sim_add(last_print_s, last_print_ns, PRINT_INTERVAL_NS, &ts, &tns);
    if (!sim_reached(ts, tns)) return;

    print_resource_table();

    last_print_s  = shm->clock.sec;
    last_print_ns = shm->clock.ns;
}

/* ════════════════════════════════════════════════════════════════
   Statistics
   ════════════════════════════════════════════════════════════════ */

static void print_statistics(void)
{
    double pct = (stat_total_req > 0)
                 ? (100.0 * stat_imm_grants / stat_total_req) : 0.0;
    printf("\n=== OSS End-of-Run Statistics ===\n");
    printf("Total resource requests:           %ld\n", stat_total_req);
    printf("Immediately granted:               %ld (%.1f%%)\n",
           stat_imm_grants, pct);
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

/* ════════════════════════════════════════════════════════════════
   IPC setup / teardown
   ════════════════════════════════════════════════════════════════ */

static void cleanup_ipc(void)
{
    if (shm)          shmdt(shm);
    if (shmid != -1)  shmctl(shmid, IPC_RMID, NULL);
    if (msgid != -1)  msgctl(msgid, IPC_RMID, NULL);
    if (logfp)        fclose(logfp);
    shm   = NULL;
    shmid = -1;
    msgid = -1;
    logfp = NULL;
}

static void sig_handler(int sig)
{
    (void)sig;
    /* kill all children */
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

static void setup_ipc(void)
{
    shmid = shmget(SHM_KEY, sizeof(SharedMem), IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); exit(1); }
    shm = (SharedMem *)shmat(shmid, NULL, 0);
    if (shm == (void *)-1) { perror("shmat"); exit(1); }

    memset(shm, 0, sizeof(SharedMem));
    for (int r = 0; r < RESOURCE_CLASSES; r++)
        shm->available[r] = INSTANCES_PER;

    msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msgid == -1) { perror("msgget"); cleanup_ipc(); exit(1); }

    /* zero wait queues */
    memset(wq,      0, sizeof(wq));
    memset(wq_head, 0, sizeof(wq_head));
    memset(wq_tail, 0, sizeof(wq_tail));
    memset(wq_size, 0, sizeof(wq_size));
}

/* ════════════════════════════════════════════════════════════════
   main
   ════════════════════════════════════════════════════════════════ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-h] [-n proc] [-s simul] [-t timeLimitForChildren]\n"
            "          [-i launchIntervalMs] [-f logfile]\n",
            prog);
}

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
        case 'h': usage(argv[0]); return 0;
        case 'n': n_limit       = atoi(optarg); break;
        case 's': s_limit       = atoi(optarg); break;
        case 't': t_limit_s     = atoi(optarg); break;
        case 'i': launch_int_ns = (unsigned int)atoi(optarg) * 1000000u; break;
        case 'f': snprintf(logfile, sizeof(logfile), "%s", optarg);       break;
        default:  usage(argv[0]); return 1;
        }
    }

    /* clamp s_limit */
    if (s_limit > MAX_RUNNING) s_limit = MAX_RUNNING;

    logfp = fopen(logfile, "w");
    if (!logfp) { perror("fopen logfile"); return 1; }

    setup_ipc();

    signal(SIGINT,  sig_handler);
    signal(SIGALRM, sig_handler);
    alarm(SIGALRM_LIMIT);
    start_real = time(NULL);

    log_line("OSS started: n=%d s=%d t=%d i=%ums logfile=%s",
             n_limit, s_limit, t_limit_s, launch_int_ns / 1000000u, logfile);

    int done = 0;
    while (!done) {
        reap_zombies();
        maybe_launch_worker();
        process_messages();
        dispatch_one_turn();
        advance_clock();
        maybe_run_deadlock();
        maybe_print_tables();

        /* exit when all intended processes have been launched and finished */
        int stop_forking = (total_launched >= n_limit)
                        || ((time(NULL) - start_real) >= WALL_CLOCK_LIMIT);
        done = stop_forking && (currently_running == 0);
    }

    print_statistics();
    cleanup_ipc();
    return 0;
}
