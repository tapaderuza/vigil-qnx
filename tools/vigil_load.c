/*
 * vigil-load - a controlled CPU load for the delivery trial (docs/DELIVERY.md).
 *
 * Starts N threads at a fixed SCHED_FIFO priority. Each thread alternates a busy burst with a short sleep:
 *
 *   vigil-load -n 2 -p 15 -b 8 -g 2 -t 120
 *
 * With the defaults (priority 15) the threads outrank vigil-watch and the HTTP thread (10) but not the
 * ingest thread (20), the resource manager (25) or the engine (40), so the load delays the consumer
 * without starving the daemon. The sleep makes the worst wait of a lower-priority thread about one burst
 * instead of "forever", which is what makes the load repeatable.
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned g_burst_ms = 8, g_gap_ms = 2;
static uint64_t g_until_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *hog(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t t = now_ns();
        if (t >= g_until_ns) break;
        const uint64_t end = t + (uint64_t)g_burst_ms * 1000000ull;
        while (now_ns() < end) {
            /* burn */
        }
        struct timespec gap = { 0, (long)g_gap_ms * 1000000L };
        nanosleep(&gap, NULL);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    unsigned threads = 2, seconds = 120;
    int prio = 15, opt;
    while ((opt = getopt(argc, argv, "n:p:b:g:t:h")) != -1) {
        switch (opt) {
        case 'n': threads = (unsigned)atoi(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'b': g_burst_ms = (unsigned)atoi(optarg); break;
        case 'g': g_gap_ms = (unsigned)atoi(optarg); break;
        case 't': seconds = (unsigned)atoi(optarg); break;
        default:
            fprintf(stderr, "usage: vigil-load [-n threads] [-p priority] [-b burst_ms] [-g gap_ms] [-t seconds]\n");
            return opt == 'h' ? 0 : 2;
        }
    }
    if (threads < 1 || threads > 16 || prio < 1 || prio > 63 || g_burst_ms < 1 || g_gap_ms < 1 || seconds < 1) {
        fprintf(stderr, "vigil-load: threads 1..16, priority 1..63, burst/gap >= 1 ms, seconds >= 1\n");
        return 2;
    }
    g_until_ns = now_ns() + (uint64_t)seconds * 1000000000ull;

    pthread_t th[16];
    unsigned started = 0, fifo = 0;
    for (unsigned i = 0; i < threads; i++) {
        pthread_attr_t attr;
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = prio;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &sp);
        int rc = pthread_create(&th[started], &attr, hog, NULL);
        if (rc == 0) {
            fifo++;
        } else {
            pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
            rc = pthread_create(&th[started], &attr, hog, NULL);
        }
        pthread_attr_destroy(&attr);
        if (rc == 0) started++;
    }
    printf("vigil-load: %u threads (%u at SCHED_FIFO %d), burst %u ms, gap %u ms, %u s\n", started, fifo, prio,
           g_burst_ms, g_gap_ms, seconds);
    fflush(stdout);
    for (unsigned i = 0; i < started; i++) pthread_join(th[i], NULL);
    return started == threads ? 0 : 1;
}
