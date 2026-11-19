/*
 * Vigil - bed state, alarm engine, event ring and renderers.
 *
 * Alarm model, after the concepts of IEC 60601-1-8 (alarm systems in medical electrical equipment):
 *  - three priorities: HIGH (immediate response), MEDIUM (prompt response), LOW (awareness);
 *  - a *condition delay*: a limit must stay violated for `onset` before the alarm is raised, so a
 *    single noisy sample never wakes a nurse, and a *release delay* before it is lowered;
 *  - *technical* alarms are separate from physiological ones: when a sensor stops delivering (or a
 *    whole bed goes silent) the monitor says so and stops judging that value, because showing the
 *    last known number as if it were current is the dangerous failure;
 *  - an audio *pause* silences the sound for a fixed time but never the visual indication.
 *
 * Portable, allocation-free C11. Vigil is a software exercise, not a medical device.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VG_ALARMS_H
#define VG_ALARMS_H

#include <stddef.h>
#include <stdint.h>

#include "vitals.h"

typedef enum { VG_LVL_NONE = 0, VG_LVL_LOW = 1, VG_LVL_MEDIUM = 2, VG_LVL_HIGH = 3 } vg_level_t;
const char *vg_level_name(vg_level_t l);

enum { VG_SRC_NEWS2 = VG_PARAM_COUNT, VG_SRC_SENSOR, VG_SRC_OFFLINE, VG_SRC_COUNT };
const char *vg_src_name(int src);

/* ------------------------------------------------------------------- configuration */

typedef struct {
    double low_med, low_high;   /* value below low_med -> MEDIUM, below low_high -> HIGH */
    double high_med, high_high; /* value above high_med -> MEDIUM, above high_high -> HIGH */
} vg_limits_t;

typedef struct {
    vg_limits_t lim[VG_PARAM_COUNT];
    uint64_t    onset_ms;   /* a violation must persist this long before the alarm is raised */
    uint64_t    clear_ms;   /* and the value must stay back in range this long before it is lowered */
    uint64_t    sensor_ms;  /* a parameter older than this is a sensor problem, not a value */
    uint64_t    offline_ms; /* a bed with no frame for this long is offline */
    uint64_t    pause_ms;   /* duration of an audio pause */
    uint64_t    news2_clear_ms; /* an aggregate score is only lowered after this long (it is not a per-second value) */
} vg_alarm_cfg_t;

void vg_alarm_cfg_default(vg_alarm_cfg_t *c);
/* "hr_high_med 120", "spo2_low_high 88", "onset_s 3", "clear_s 5", "sensor_s 5", "offline_s 10", "pause_s 120". */
int  vg_alarm_cfg_apply(vg_alarm_cfg_t *c, const char *line, char *err, size_t errlen);

/* -------------------------------------------------------------------------- beds */

typedef struct {
    uint8_t  seen;
    uint64_t t_frame_ms;         /* last frame of any kind */
    double   v[VG_PARAM_COUNT];
    uint8_t  known[VG_PARAM_COUNT]; /* delivered at least once: only these can raise a sensor alarm */
    uint8_t  has[VG_PARAM_COUNT];
    uint64_t t_v[VG_PARAM_COUNT];
    char     acvpu;
    int8_t   o2;
    uint64_t t_ctx_ms;           /* when acvpu/o2 were last delivered */
    uint32_t frames;
} vg_bed_t;

typedef struct { vg_bed_t bed[VG_MAX_BEDS]; } vg_beds_t;

void vg_beds_init(vg_beds_t *b);
void vg_beds_apply(vg_beds_t *b, const vg_sample_t *s, uint64_t now_ms);

/* ------------------------------------------------------------------------ engine */

typedef struct { vg_level_t level, cand; uint64_t cand_since, ok_since; uint8_t ok_active; } vg_src_state_t;

typedef struct {
    vg_alarm_cfg_t cfg;
    vg_src_state_t st[VG_MAX_BEDS][VG_SRC_COUNT];
    uint64_t       seq;
    uint64_t       pause_until_ms;
} vg_engine_t;

void vg_engine_init(vg_engine_t *e, const vg_alarm_cfg_t *cfg);
void vg_engine_pause_audio(vg_engine_t *e, uint64_t now_ms);   /* start an audio pause */
void vg_engine_resume_audio(vg_engine_t *e);

typedef struct {
    uint8_t    seen, online;
    uint32_t   age_ms;
    uint8_t    valid[VG_PARAM_COUNT];
    double     v[VG_PARAM_COUNT];
    char       acvpu;
    int8_t     o2;
    vg_news2_t news2;
    vg_level_t src_level[VG_SRC_COUNT];
    uint8_t    stale_mask;  /* parameters that were known but are no longer delivered */
    vg_level_t level;       /* highest of the sources */
    uint8_t    audible;     /* level > 0 and audio not paused */
} vg_bed_view_t;

typedef struct {
    uint64_t      seq, t_ms;
    vg_bed_view_t bed[VG_MAX_BEDS];
    uint32_t      n_high, n_medium, n_low; /* beds by highest level */
    vg_level_t    highest;
    uint8_t       audio_paused;
    uint32_t      pause_remaining_s;
    /* Health counters, filled by the daemon. */
    uint64_t cycles, overruns, samples, bad_frames;
    uint32_t period_us, last_cycle_us, max_cycle_us, last_latency_us, max_latency_us, subscribers;
} vg_picture_t;

typedef struct { uint8_t bed, src, from, to; } vg_transition_t;

uint32_t vg_engine_step(vg_engine_t *e, const vg_beds_t *beds, uint64_t now_ms, vg_picture_t *pic,
                        vg_transition_t *trans, uint32_t max_trans);

/* ------------------------------------------------------------------------ events */

#define VG_EVENT_RING 128
#define VG_EVENTS_FROM_NOW UINT64_MAX

typedef struct {
    uint64_t seq, t_ms;
    uint8_t  bed, src, from, to;
    float    value;   /* the triggering value (NEWS2: the score) */
    uint8_t  value_ok;
} vg_event_t;

typedef struct { vg_event_t ev[VG_EVENT_RING]; uint64_t next_seq; } vg_events_t;

void     vg_events_init(vg_events_t *r);
void     vg_events_push(vg_events_t *r, uint64_t t_ms, const vg_transition_t *tr, uint32_t n,
                        const vg_picture_t *pic);
uint32_t vg_events_since(const vg_events_t *r, uint64_t since_seq, vg_event_t *out, uint32_t max,
                         uint32_t *lost);

/* ---------------------------------------------------------------------- renderers */

size_t vg_render_beds(const vg_picture_t *p, char *buf, size_t len);
size_t vg_render_alarms(const vg_picture_t *p, const vg_alarm_cfg_t *c, char *buf, size_t len);
size_t vg_render_stats(const vg_picture_t *p, char *buf, size_t len);
size_t vg_render_config(const vg_alarm_cfg_t *c, char *buf, size_t len);
size_t vg_render_events(const vg_event_t *ev, uint32_t n, char *buf, size_t len);
size_t vg_render_json(const vg_picture_t *p, const vg_alarm_cfg_t *c, char *buf, size_t len);

#endif /* VG_ALARMS_H */
