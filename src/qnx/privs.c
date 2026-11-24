/*
 * Least privilege for the alarm engine, with QNX process-manager abilities.
 *
 * vigild needs root only to set itself up: a public channel, the /dev/vigil path space, sockets,
 * SCHED_FIFO priorities and locked memory. Once every thread has its resources (each one calls
 * vgd_ready()), vgd_lockdown() denies and LOCKS, for root and non-root alike, every ability an
 * attacker would want from a compromised bedside monitor: starting programs, new path names,
 * physical memory and port I/O, interrupts, setting the clock, rebooting, reading or debugging
 * other processes, changing user. Locked abilities cannot be re-enabled, even by root.
 * It then proves it: a spawn attempt must fail, and the result goes to slog2.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <sys/procmgr.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vigild.h"

static pthread_mutex_t g_ready_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;

void vgd_ready(void)
{
    pthread_mutex_lock(&g_ready_lock);
    g_ready++;
    pthread_mutex_unlock(&g_ready_lock);
}

static int ready_count(void)
{
    pthread_mutex_lock(&g_ready_lock);
    int n = g_ready;
    pthread_mutex_unlock(&g_ready_lock);
    return n;
}

#define DENY_LOCK (PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK)

int vgd_lockdown(int expected_ready)
{
    /* Start-up only: wait (up to 3 s) until every thread holds its resources. */
    for (int i = 0; i < 300 && ready_count() < expected_ready; i++) {
        usleep(10000);
    }
    int got = ready_count();
    if (got < expected_ready) {
        vgd_log(SLOG2_WARNING, "lockdown: %d of %d threads ready after 3 s; locking anyway", got, expected_ready);
    }
    int rc = procmgr_ability(0,
                             DENY_LOCK | PROCMGR_AID_SPAWN,
                             DENY_LOCK | PROCMGR_AID_FORK,
                             DENY_LOCK | PROCMGR_AID_SPAWN_SETUID,
                             DENY_LOCK | PROCMGR_AID_SPAWN_SETGID,
                             DENY_LOCK | PROCMGR_AID_SETUID,
                             DENY_LOCK | PROCMGR_AID_SETGID,
                             DENY_LOCK | PROCMGR_AID_PATHSPACE,
                             DENY_LOCK | PROCMGR_AID_REBOOT,
                             DENY_LOCK | PROCMGR_AID_MEM_PHYS,
                             DENY_LOCK | PROCMGR_AID_MEM_ADD,
                             DENY_LOCK | PROCMGR_AID_MEM_PEER,
                             DENY_LOCK | PROCMGR_AID_IO,
                             DENY_LOCK | PROCMGR_AID_INTERRUPT,
                             DENY_LOCK | PROCMGR_AID_CLOCKSET,
                             DENY_LOCK | PROCMGR_AID_RSRCDBMGR,
                             DENY_LOCK | PROCMGR_AID_TRACE,
                             DENY_LOCK | PROCMGR_AID_CHROOT,
                             DENY_LOCK | PROCMGR_AID_XPROCESS_DEBUG,
                             DENY_LOCK | PROCMGR_AID_XPROCESS_MEM_READ,
                             DENY_LOCK | PROCMGR_AID_ABLE_CREATE,
                             PROCMGR_AID_EOL);
    if (rc != EOK) {
        vgd_log(SLOG2_ERROR, "lockdown: procmgr_ability failed: %s", strerror(rc));
        return -1;
    }

    /* Prove it: starting any program must now be refused by the process manager. */
    pid_t pid;
    char *const argv[] = {"true", NULL};
    char *const envp[] = {NULL};
    int sp = posix_spawn(&pid, "/proc/boot/true", NULL, NULL, argv, envp);
    if (sp == 0) {
        waitpid(pid, NULL, 0);
        vgd_log(SLOG2_ERROR, "lockdown: spawn still allowed; check the abilities");
        return -1;
    }
    vgd_log(SLOG2_NOTICE,
            "lockdown: spawn, fork, setuid, new path names, physical memory, port I/O, interrupts, clock, reboot "
            "and cross-process debug denied and locked (spawn test: %s)",
            strerror(sp));
    return 0;
}
