/*
 * vigild - internal shared declarations (QNX 8.0 target).
 *
 * Thread layout and priorities (QNX priorities 1..63, SCHED_FIFO):
 *
 *   engine   40  owns all state; 10 Hz timer pulse; receives all messages
 *   resmgr   25  resource manager for the /dev/vigil devices (reads are snapshots)
 *   ingest   20  UDP socket -> frame decode -> MsgSend to the engine
 *   http     10  read-only bedside dashboard + JSON API
 *
 * The engine is the only writer of the picture. Other threads read it through vgd_pub_snapshot(),
 * a short critical section guarded by a priority-inheriting mutex, so a low-priority reader can
 * never hold up the engine for longer than one memcpy.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VG_VIGILD_H
#define VG_VIGILD_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/slog2.h>

#include "../core/alarms.h"
#include "vigil_ipc.h"

typedef struct {
    int      udp_port;
    char     mcast_group[32]; /* "" = unicast only */
    int      http_port;       /* 0 = disabled */
    char     http_bind[32];
    unsigned period_ms;
    int      prio_engine, prio_resmgr, prio_ingest, prio_http;
    char     dev_prefix[48];  /* "/dev/vigil" */
    int      foreground;      /* also log to stderr */
    int      no_lockdown;     /* -L: keep all abilities (debugging only) */
} vgd_options_t;

extern vgd_options_t g_opt;
extern int           g_engine_chid;

/* Counters written by the ingest thread, read by the engine when it publishes. */
typedef struct {
    _Atomic uint64_t samples, bad_frames;
} vgd_counters_t;
extern vgd_counters_t g_cnt;

/* The published, read-only view of the ward. */
typedef struct {
    vg_picture_t   pic;
    vg_alarm_cfg_t cfg;
    vg_events_t    events;
} vgd_pub_t;

/* Copy the latest published state into *dst. Safe from any thread. */
void vgd_pub_snapshot(vgd_pub_t *dst);

/* Logging: slog2 (read with `slog2info -b vigil`), optionally mirrored to stderr. */
extern slog2_buffer_t g_slog;
void vgd_log(int severity, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

uint64_t vgd_now_ns(void);

/* Start a SCHED_FIFO thread. Falls back to inherited priority (with a warning) if refused. */
int vgd_spawn(pthread_t *t, int prio, const char *name, void *(*fn)(void *));

/* Least privilege (privs.c): each thread calls vgd_ready() once it holds its resources;
 * main then calls vgd_lockdown() to deny and lock every ability the running engine does not need. */
void vgd_ready(void);
int vgd_lockdown(int expected_ready);

void *vgd_ingest_thread(void *arg);
void *vgd_resmgr_thread(void *arg);
void *vgd_http_thread(void *arg);

#endif /* VG_VIGILD_H */
