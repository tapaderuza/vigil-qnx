/*
 * vigild - internal shared declarations (QNX 8.0 target only).
 *
 * Thread layout and priorities (QNX priorities 1..63, SCHED_FIFO):
 *
 *   engine   40  owns the picture; 10 Hz timer pulse; receives all messages
 *   resmgr   25  resource manager for the /dev/vigil devices (reads are snapshots)
 *   ingest   20  UDP socket -> NMEA/AIS decode -> MsgSend to the engine
 *   http     10  read-only dashboard + JSON API
 *
 * The engine is the only writer of the picture. Other threads read it
 * through lkd_pub_snapshot(), a short critical section guarded by a
 * priority-inheriting mutex, so a low-priority reader can never hold up
 * the engine for longer than one memcpy.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VG_LOOKOUTD_H
#define VG_LOOKOUTD_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/slog2.h>

#include "../core/engine.h"
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
} lkd_options_t;

extern lkd_options_t g_opt;
extern int           g_engine_chid;

/* Counters written by the ingest thread, read by the engine when it publishes. */
typedef struct {
    _Atomic uint64_t sentences, bad_sentences, ais_msgs, ais_errors, ownship_fixes;
} lkd_counters_t;
extern lkd_counters_t g_cnt;

/* The published, read-only view of the world. */
typedef struct {
    vg_picture_t   pic;
    vg_alarm_cfg_t cfg;
    vg_events_t    events;
} lkd_pub_t;

/* Copy the latest published state into *dst. Safe from any thread. */
void lkd_pub_snapshot(lkd_pub_t *dst);

/* Logging: slog2 (read with `slog2info -b vigil`), optionally mirrored to stderr. */
extern slog2_buffer_t g_slog;
void lkd_log(int severity, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

uint64_t lkd_now_ns(void);

/* Start a SCHED_FIFO thread. Falls back to inherited priority (with a warning) if refused. */
int lkd_spawn(pthread_t *t, int prio, const char *name, void *(*fn)(void *));

void *lkd_ingest_thread(void *arg);
void *lkd_resmgr_thread(void *arg);
void *lkd_http_thread(void *arg);

#endif /* VG_LOOKOUTD_H */
