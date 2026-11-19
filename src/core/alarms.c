/*
 * Vigil - alarm engine, event ring and renderers.
 * SPDX-License-Identifier: MIT
 */
#include "alarms.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *vg_level_name(vg_level_t l)
{
    switch (l) {
    case VG_LVL_LOW: return "LOW";
    case VG_LVL_MEDIUM: return "MEDIUM";
    case VG_LVL_HIGH: return "HIGH";
    default: return "NONE";
    }
}

const char *vg_src_name(int src)
{
    static const char *const nm[VG_SRC_COUNT] = { "HR", "SpO2", "RR", "SBP", "TEMP", "NEWS2", "SENSOR", "OFFLINE" };
    return (src >= 0 && src < VG_SRC_COUNT) ? nm[src] : "?";
}

const char *vg_obs_name(int i)
{
    static const char *const nm[VG_OBS_COUNT] = { "HR", "SpO2", "RR", "SBP", "TEMP", "ACVPU", "O2" };
    return (i >= 0 && i < VG_OBS_COUNT) ? nm[i] : "?";
}

/* ----------------------------------------------------------------------- config */

void vg_alarm_cfg_default(vg_alarm_cfg_t *c)
{
    const vg_limits_t d[VG_PARAM_COUNT] = {
        /* HR   */ { 50.0, 40.0, 120.0, 140.0 },
        /* SpO2 */ { 92.0, 88.0, 1e9, 1e9 },
        /* RR   */ { 8.0, 6.0, 24.0, 30.0 },
        /* SBP  */ { 100.0, 90.0, 180.0, 220.0 },
        /* TEMP */ { 35.5, 34.0, 38.5, 40.0 },
    };
    memcpy(c->lim, d, sizeof(d));
    c->onset_ms = 3000;
    c->clear_ms = 5000;
    c->sensor_ms = 5000;
    c->offline_ms = 10000;
    c->pause_ms = 120000;
    c->news2_clear_ms = 30000;
}

int vg_alarm_cfg_apply(vg_alarm_cfg_t *c, const char *line, char *err, size_t errlen)
{
    char key[32];
    double v;
    if (sscanf(line, " %31s %lf", key, &v) != 2) {
        snprintf(err, errlen, "expected '<key> <number>'");
        return -1;
    }
    if (!isfinite(v) || v <= 0.0) {
        snprintf(err, errlen, "%s must be a positive number", key);
        return -1;
    }
    vg_alarm_cfg_t n = *c;
    int done = 0;
    static const char *const suffix[4] = { "_low_med", "_low_high", "_high_med", "_high_high" };
    for (int p = 0; p < VG_PARAM_COUNT && !done; p++) {
        for (int s = 0; s < 4 && !done; s++) {
            char name[40], lower[8];
            snprintf(lower, sizeof(lower), "%s", vg_param_name(p));
            for (char *q = lower; *q; q++) {
                if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
            }
            snprintf(name, sizeof(name), "%s%s", lower, suffix[s]);
            if (strcmp(key, name) == 0) {
                double *f[4] = { &n.lim[p].low_med, &n.lim[p].low_high, &n.lim[p].high_med, &n.lim[p].high_high };
                *f[s] = v;
                done = 1;
            }
        }
    }
    if (!done) {
        if (strcmp(key, "onset_s") == 0) n.onset_ms = (uint64_t)(v * 1000.0);
        else if (strcmp(key, "clear_s") == 0) n.clear_ms = (uint64_t)(v * 1000.0);
        else if (strcmp(key, "sensor_s") == 0) n.sensor_ms = (uint64_t)(v * 1000.0);
        else if (strcmp(key, "offline_s") == 0) n.offline_ms = (uint64_t)(v * 1000.0);
        else if (strcmp(key, "pause_s") == 0) n.pause_ms = (uint64_t)(v * 1000.0);
        else if (strcmp(key, "news2_clear_s") == 0) n.news2_clear_ms = (uint64_t)(v * 1000.0);
        else {
            snprintf(err, errlen, "unknown key '%s'", key);
            return -1;
        }
    }
    for (int p = 0; p < VG_PARAM_COUNT; p++) {
        const vg_limits_t *l = &n.lim[p];
        if (l->low_high > l->low_med || l->high_med > l->high_high || l->low_med > l->high_med) {
            snprintf(err, errlen, "%s limits must satisfy low_high <= low_med <= high_med <= high_high",
                     vg_param_name(p));
            return -1;
        }
    }
    if (n.offline_ms < n.sensor_ms || n.pause_ms > 600000) {
        snprintf(err, errlen, "need offline_s >= sensor_s and pause_s <= 600");
        return -1;
    }
    *c = n;
    return 0;
}

/* ------------------------------------------------------------------------ beds */

void vg_beds_init(vg_beds_t *b)
{
    memset(b, 0, sizeof(*b));
    for (int i = 0; i < VG_MAX_BEDS; i++) {
        b->bed[i].o2 = -1;
    }
}

void vg_beds_apply(vg_beds_t *b, const vg_sample_t *s, uint64_t now_ms)
{
    if (s->bed < 1 || s->bed > VG_MAX_BEDS) {
        return;
    }
    vg_bed_t *bd = &b->bed[s->bed - 1];
    bd->seen = 1;
    bd->t_frame_ms = now_ms;
    bd->frames++;
    for (int p = 0; p < VG_PARAM_COUNT; p++) {
        if (s->has[p]) { /* a missing field keeps the old value, which then ages into a sensor alarm */
            bd->v[p] = s->v[p];
            bd->known[p] = 1;
            bd->t_v[p] = now_ms;
        }
    }
    /* Each observation has its own clock: one that keeps arriving must never vouch for the other. */
    if (s->acvpu != 0) {
        bd->acvpu = s->acvpu;
        bd->t_acvpu_ms = now_ms;
    }
    if (s->o2 >= 0) {
        bd->o2 = s->o2;
        bd->t_o2_ms = now_ms;
    }
}

/* ----------------------------------------------------------------------- engine */

void vg_engine_init(vg_engine_t *e, const vg_alarm_cfg_t *cfg)
{
    memset(e, 0, sizeof(*e));
    if (cfg != NULL) e->cfg = *cfg; else vg_alarm_cfg_default(&e->cfg);
}

void vg_engine_pause_audio(vg_engine_t *e, uint64_t now_ms) { e->pause_until_ms = now_ms + e->cfg.pause_ms; }
void vg_engine_resume_audio(vg_engine_t *e) { e->pause_until_ms = 0; }

static vg_level_t level_for(const vg_limits_t *l, double v)
{
    if (v < l->low_high || v > l->high_high) return VG_LVL_HIGH;
    if (v < l->low_med || v > l->high_med) return VG_LVL_MEDIUM;
    return VG_LVL_NONE;
}

/*
 * Value hysteresis: once an alarm is active the limits are widened by a small margin, so a value
 * that hovers on a limit (120, 121, 120...) cannot make the alarm flicker on and off.
 */
static vg_level_t level_with_hysteresis(const vg_limits_t *l, double h, double v, vg_level_t active)
{
    vg_level_t raw = level_for(l, v);
    if (active == VG_LVL_NONE) return raw;
    vg_limits_t r = { l->low_med + h, l->low_high + h, l->high_med - h, l->high_high - h };
    vg_level_t held = level_for(&r, v);
    if (held > active) held = active;
    return held > raw ? held : raw;
}

static const double k_hyst[VG_PARAM_COUNT] = { 3.0, 1.0, 1.0, 4.0, 0.2 }; /* bpm, %, /min, mmHg, C */

/* Persistence logic shared by every source: raise after `onset`, lower after `clear`. */
static void update_src(vg_src_state_t *s, vg_level_t want, uint64_t now, uint64_t onset, uint64_t clear)
{
    if (want > s->level) {
        if (s->cand != want) {
            s->cand = want;
            s->cand_since = now;
        }
        s->ok_active = 0;
        if (now - s->cand_since >= onset) {
            s->level = want;
        }
    } else if (want == s->level) {
        s->cand = want;
        s->ok_active = 0;
    } else {
        s->cand = want;
        if (!s->ok_active) {
            s->ok_active = 1;
            s->ok_since = now;
        }
        if (now - s->ok_since >= clear) {
            s->level = want;
            s->ok_active = 0;
        }
    }
}

uint32_t vg_engine_step(vg_engine_t *e, const vg_beds_t *beds, uint64_t now_ms, vg_picture_t *pic,
                        vg_transition_t *trans, uint32_t max_trans)
{
    const vg_alarm_cfg_t *c = &e->cfg;
    uint32_t ntr = 0;

    pic->seq = ++e->seq;
    pic->t_ms = now_ms;
    memset(pic->bed, 0, sizeof(pic->bed));
    pic->n_high = pic->n_medium = pic->n_low = 0;
    pic->highest = VG_LVL_NONE;
    pic->audio_paused = now_ms < e->pause_until_ms;
    pic->pause_remaining_s = pic->audio_paused ? (uint32_t)((e->pause_until_ms - now_ms + 999) / 1000) : 0;

    for (int i = 0; i < VG_MAX_BEDS; i++) {
        const vg_bed_t *bd = &beds->bed[i];
        vg_bed_view_t *v = &pic->bed[i];
        if (!bd->seen) {
            continue;
        }
        v->seen = 1;
        uint64_t age = now_ms >= bd->t_frame_ms ? now_ms - bd->t_frame_ms : 0;
        v->age_ms = (uint32_t)(age > 0xffffffffu ? 0xffffffffu : age);
        v->online = age <= c->offline_ms;
        v->acvpu = bd->acvpu;
        v->o2 = bd->o2;

        vg_level_t want[VG_SRC_COUNT];
        memset(want, 0, sizeof(want));
        int all_valid = 1;
        for (int p = 0; p < VG_PARAM_COUNT; p++) {
            v->v[p] = bd->v[p];
            uint64_t pa = now_ms >= bd->t_v[p] ? now_ms - bd->t_v[p] : 0;
            v->valid[p] = v->online && bd->known[p] && pa <= c->sensor_ms;
            if (v->online && bd->known[p] && !v->valid[p]) {
                v->stale_mask |= (uint8_t)(1u << p);
            }
            if (!v->valid[p]) all_valid = 0;
            want[p] = v->valid[p] ? level_with_hysteresis(&c->lim[p], k_hyst[p], bd->v[p], e->st[i][p].level)
                                  : VG_LVL_NONE;
        }
        /*
         * Consciousness and oxygen are judged exactly like the vital signs: delivered, and not older
         * than sensor_ms by their own clock. One that expired makes NEWS2 not calculable; once it was
         * known it also counts as a sensor not delivering, so an active NEWS2 alarm cannot just vanish.
         */
        uint64_t aa = now_ms >= bd->t_acvpu_ms ? now_ms - bd->t_acvpu_ms : 0;
        uint64_t oa = now_ms >= bd->t_o2_ms ? now_ms - bd->t_o2_ms : 0;
        v->acvpu_valid = v->online && bd->acvpu != 0 && aa <= c->sensor_ms;
        v->o2_valid = v->online && bd->o2 >= 0 && oa <= c->sensor_ms;
        if (v->online && bd->acvpu != 0 && !v->acvpu_valid) v->stale_mask |= (uint8_t)(1u << VG_OBS_ACVPU);
        if (v->online && bd->o2 >= 0 && !v->o2_valid) v->stale_mask |= (uint8_t)(1u << VG_OBS_O2);

        want[VG_SRC_SENSOR] = v->stale_mask ? VG_LVL_MEDIUM : VG_LVL_NONE;
        want[VG_SRC_OFFLINE] = v->online ? VG_LVL_NONE : VG_LVL_MEDIUM;

        if (v->online && all_valid && v->acvpu_valid && v->o2_valid) {
            uint8_t has[VG_PARAM_COUNT] = { 1, 1, 1, 1, 1 };
            vg_news2_compute(bd->v, has, bd->acvpu, bd->o2, &v->news2);
            if (v->news2.valid) {
                want[VG_SRC_NEWS2] = v->news2.total >= 7 ? VG_LVL_HIGH : v->news2.total >= 5 ? VG_LVL_MEDIUM
                                   : v->news2.any3 ? VG_LVL_LOW : VG_LVL_NONE;
            }
        }

        for (int s = 0; s < VG_SRC_COUNT; s++) {
            vg_src_state_t *st = &e->st[i][s];
            vg_level_t before = st->level;
            /*
             * Technical alarms are raised at once: the delay exists to ride out noisy values. And a
             * source that can no longer be judged (sensor off, bed silent) is closed at once too, so
             * a stale number never stays on screen as a live alarm.
             */
            int judged = s < VG_PARAM_COUNT ? v->valid[s] : (s == VG_SRC_NEWS2 ? v->news2.valid : 0);
            /* Technical alarms also end as soon as their cause does (probe back on, bed answering). */
            uint64_t clear = !judged || s >= VG_SRC_SENSOR ? 0 : (s == VG_SRC_NEWS2 ? c->news2_clear_ms : c->clear_ms);
            update_src(st, want[s], now_ms, s >= VG_SRC_SENSOR ? 0 : c->onset_ms, clear);
            if (st->level != before && trans != NULL && ntr < max_trans) {
                trans[ntr].bed = (uint8_t)(i + 1);
                trans[ntr].src = (uint8_t)s;
                trans[ntr].from = (uint8_t)before;
                trans[ntr].to = (uint8_t)st->level;
                /* when the condition behind this change first appeared: the start of the onset or the release delay */
                trans[ntr].since_ms = st->level > before ? st->cand_since : st->ok_since;
                ntr++;
            }
            v->src_level[s] = st->level;
            if (st->level > v->level) v->level = st->level;
        }
        v->audible = v->level > VG_LVL_NONE && !pic->audio_paused;
        if (v->level == VG_LVL_HIGH) pic->n_high++;
        else if (v->level == VG_LVL_MEDIUM) pic->n_medium++;
        else if (v->level == VG_LVL_LOW) pic->n_low++;
        if (v->level > pic->highest) pic->highest = v->level;
    }
    return ntr;
}

/* ----------------------------------------------------------------------- events */

void vg_events_init(vg_events_t *r)
{
    memset(r, 0, sizeof(*r));
    r->next_seq = 1;
}

void vg_events_push(vg_events_t *r, uint64_t t_ms, const vg_transition_t *tr, uint32_t n,
                    const vg_picture_t *pic)
{
    for (uint32_t i = 0; i < n; i++) {
        vg_event_t *e = &r->ev[r->next_seq % VG_EVENT_RING];
        memset(e, 0, sizeof(*e));
        e->seq = r->next_seq++;
        e->t_ms = t_ms;
        e->bed = tr[i].bed;
        e->src = tr[i].src;
        e->from = tr[i].from;
        e->to = tr[i].to;
        e->cond_ms = tr[i].since_ms;
        if (tr[i].bed >= 1 && tr[i].bed <= VG_MAX_BEDS) {
            const vg_bed_view_t *v = &pic->bed[tr[i].bed - 1];
            if (tr[i].src < VG_PARAM_COUNT) {
                e->value = (float)v->v[tr[i].src];
                e->value_ok = 1;
            } else if (tr[i].src == VG_SRC_NEWS2 && v->news2.valid) {
                e->value = (float)v->news2.total;
                e->value_ok = 1;
            }
        }
    }
}

void vg_events_stamp(vg_events_t *r, uint64_t first_seq, uint32_t n, uint64_t t_decided_ns, uint64_t t_notified_ns)
{
    for (uint32_t i = 0; i < n; i++) {
        uint64_t seq = first_seq + i;
        if (seq == 0 || seq >= r->next_seq) break;
        vg_event_t *e = &r->ev[seq % VG_EVENT_RING];
        if (e->seq != seq) continue; /* already overwritten by a newer event */
        if (t_decided_ns != 0) e->t_decided_ns = t_decided_ns;
        if (t_notified_ns != 0) e->t_notified_ns = t_notified_ns;
    }
}

uint32_t vg_events_since(const vg_events_t *r, uint64_t since_seq, vg_event_t *out, uint32_t max,
                         uint32_t *lost)
{
    uint64_t newest = r->next_seq - 1;
    uint64_t oldest = newest >= VG_EVENT_RING ? newest - VG_EVENT_RING + 1 : 1;
    uint32_t missed = 0;
    if (since_seq == VG_EVENTS_FROM_NOW) {
        if (lost != NULL) *lost = 0;
        return 0;
    }
    uint64_t first = since_seq + 1;
    if (first < oldest) {
        missed = (uint32_t)(oldest - first);
        first = oldest;
    }
    uint32_t n = 0;
    for (uint64_t s = first; s <= newest && n < max; s++) {
        out[n++] = r->ev[s % VG_EVENT_RING];
    }
    if (lost != NULL) *lost = missed;
    return n;
}

/* -------------------------------------------------------------------- renderers */

typedef struct { char *buf; size_t len, pos; } sbuf_t;

static void sb_printf(sbuf_t *sb, const char *fmt, ...)
{
    if (sb->len == 0 || sb->pos >= sb->len - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->pos, sb->len - sb->pos, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb->pos += (size_t)n;
    if (sb->pos >= sb->len) sb->pos = sb->len - 1;
}

#define SB_INIT(name, b, l) sbuf_t name = { (b), (l), 0 }; do { if ((l) > 0) (b)[0] = '\0'; } while (0)

static void cell(sbuf_t *sb, const vg_bed_view_t *v, int p, const char *f, int width)
{
    char t[24];
    if (v->valid[p]) snprintf(t, sizeof(t), f, v->v[p]); else snprintf(t, sizeof(t), "--");
    sb_printf(sb, "%*s", width, t);
}

size_t vg_render_beds(const vg_picture_t *p, char *buf, size_t len)
{
    SB_INIT(sb, buf, len);
    sb_printf(&sb, "%-4s %5s %5s %5s %5s %6s  %-5s %-10s %s\n", "BED", "HR", "SpO2", "RR", "SBP", "TEMP",
              "NEWS2", "RISK", "ALARM");
    for (int i = 0; i < VG_MAX_BEDS; i++) {
        const vg_bed_view_t *v = &p->bed[i];
        if (!v->seen) continue;
        sb_printf(&sb, "%-4d", i + 1);
        cell(&sb, v, VG_HR, "%.0f", 6); cell(&sb, v, VG_SPO2, "%.0f", 6); cell(&sb, v, VG_RR, "%.0f", 6);
        cell(&sb, v, VG_SBP, "%.0f", 6); cell(&sb, v, VG_TEMP, "%.1f", 7);
        if (v->news2.valid) sb_printf(&sb, "  %-5d %-10s", v->news2.total, vg_news2_band(&v->news2));
        else sb_printf(&sb, "  %-5s %-10s", "--", "N/A");
        sb_printf(&sb, " %s%s\n", v->online ? vg_level_name(v->level) : "OFFLINE", v->audible ? " (audible)" : "");
    }
    return sb.pos;
}

size_t vg_render_alarms(const vg_picture_t *p, const vg_alarm_cfg_t *c, char *buf, size_t len)
{
    SB_INIT(sb, buf, len);
    uint32_t n = 0;
    for (int lvl = VG_LVL_HIGH; lvl >= VG_LVL_LOW; lvl--) {
        for (int i = 0; i < VG_MAX_BEDS; i++) {
            const vg_bed_view_t *v = &p->bed[i];
            if (!v->seen) continue;
            for (int s = 0; s < VG_SRC_COUNT; s++) {
                if ((int)v->src_level[s] != lvl) continue;
                n++;
                sb_printf(&sb, "%-6s bed %d  ", vg_level_name((vg_level_t)lvl), i + 1);
                if (s < VG_PARAM_COUNT) {
                    int low = v->v[s] < c->lim[s].low_med;
                    sb_printf(&sb, "%s %.*f %s %s (limit %.*f)\n", vg_param_name(s), s == VG_TEMP ? 1 : 0, v->v[s],
                              vg_param_unit(s), low ? "too low" : "too high", s == VG_TEMP ? 1 : 0,
                              low ? (lvl == VG_LVL_HIGH ? c->lim[s].low_high : c->lim[s].low_med)
                                  : (lvl == VG_LVL_HIGH ? c->lim[s].high_high : c->lim[s].high_med));
                } else if (s == VG_SRC_NEWS2) {
                    sb_printf(&sb, "NEWS2 %d (%s): clinical review\n", v->news2.total, vg_news2_band(&v->news2));
                } else if (s == VG_SRC_SENSOR) {
                    sb_printf(&sb, "TECHNICAL sensor not delivering:");
                    for (int q = 0; q < VG_OBS_COUNT; q++) {
                        if (v->stale_mask & (1u << q)) sb_printf(&sb, " %s", vg_obs_name(q));
                    }
                    sb_printf(&sb, " (values not judged%s)\n",
                              (v->stale_mask >> VG_OBS_ACVPU) ? "; NEWS2 not calculable" : "");
                } else {
                    sb_printf(&sb, "TECHNICAL no data from this bed for %u s\n", v->age_ms / 1000u);
                }
            }
        }
    }
    if (n == 0) sb_printf(&sb, "CLEAR no active alarms\n");
    if (p->audio_paused) sb_printf(&sb, "AUDIO PAUSED %u s (visual alarms unaffected)\n", p->pause_remaining_s);
    return sb.pos;
}

size_t vg_render_stats(const vg_picture_t *p, char *buf, size_t len)
{
    SB_INIT(sb, buf, len);
    uint32_t online = 0;
    for (int i = 0; i < VG_MAX_BEDS; i++) online += p->bed[i].seen && p->bed[i].online;
    sb_printf(&sb, "seq %llu\ncycles %llu\ndeadline_overruns %llu\nperiod_us %u\nlast_cycle_us %u\nmax_cycle_us %u\n"
                   "last_latency_us %u\nmax_latency_us %u\nsamples %llu\nbad_frames %llu\nbeds_online %u\nsubscribers %u\n"
                   "audio_paused %u\n",
              (unsigned long long)p->seq, (unsigned long long)p->cycles, (unsigned long long)p->overruns,
              p->period_us, p->last_cycle_us, p->max_cycle_us, p->last_latency_us, p->max_latency_us,
              (unsigned long long)p->samples, (unsigned long long)p->bad_frames, online, p->subscribers,
              p->audio_paused);
    return sb.pos;
}

size_t vg_render_config(const vg_alarm_cfg_t *c, char *buf, size_t len)
{
    SB_INIT(sb, buf, len);
    for (int p = 0; p < VG_PARAM_COUNT; p++) {
        char lower[8];
        snprintf(lower, sizeof(lower), "%s", vg_param_name(p));
        for (char *q = lower; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
        const vg_limits_t *l = &c->lim[p];
        sb_printf(&sb, "%s_low_med %g\n%s_low_high %g\n", lower, l->low_med, lower, l->low_high);
        if (l->high_med < 1e8) sb_printf(&sb, "%s_high_med %g\n%s_high_high %g\n", lower, l->high_med, lower, l->high_high);
    }
    sb_printf(&sb, "onset_s %g\nclear_s %g\nsensor_s %g\noffline_s %g\npause_s %g\nnews2_clear_s %g\n", c->onset_ms / 1000.0,
              c->clear_ms / 1000.0, c->sensor_ms / 1000.0, c->offline_ms / 1000.0, c->pause_ms / 1000.0,
              c->news2_clear_ms / 1000.0);
    return sb.pos;
}

size_t vg_render_events(const vg_event_t *ev, uint32_t n, char *buf, size_t len)
{
    SB_INIT(sb, buf, len);
    for (uint32_t i = 0; i < n; i++) {
        const vg_event_t *e = &ev[i];
        sb_printf(&sb, "#%llu t=%.1fs bed %u %-7s %s -> %s", (unsigned long long)e->seq, e->t_ms / 1000.0, e->bed,
                  vg_src_name(e->src), vg_level_name((vg_level_t)e->from), vg_level_name((vg_level_t)e->to));
        if (e->value_ok) sb_printf(&sb, "  value %.*f", e->src == VG_TEMP ? 1 : 0, e->value);
        sb_printf(&sb, "\n");
    }
    return sb.pos;
}

static void jnum(sbuf_t *sb, const vg_bed_view_t *v, int p, int dec)
{
    if (v->valid[p]) sb_printf(sb, "%.*f", dec, v->v[p]); else sb_printf(sb, "null");
}

size_t vg_render_json(const vg_picture_t *p, const vg_alarm_cfg_t *c, char *buf, size_t len)
{
    SB_INIT(sb, buf, len);
    sb_printf(&sb, "{\"seq\":%llu,\"t_ms\":%llu,\"highest\":\"%s\",\"n_high\":%u,\"n_medium\":%u,\"n_low\":%u,"
                   "\"audio\":{\"paused\":%s,\"remaining_s\":%u},",
              (unsigned long long)p->seq, (unsigned long long)p->t_ms, vg_level_name(p->highest), p->n_high,
              p->n_medium, p->n_low, p->audio_paused ? "true" : "false", p->pause_remaining_s);
    sb_printf(&sb, "\"cfg\":{\"onset_s\":%g,\"clear_s\":%g,\"sensor_s\":%g,\"offline_s\":%g},", c->onset_ms / 1000.0,
              c->clear_ms / 1000.0, c->sensor_ms / 1000.0, c->offline_ms / 1000.0);
    sb_printf(&sb, "\"health\":{\"cycles\":%llu,\"overruns\":%llu,\"period_us\":%u,\"last_cycle_us\":%u,"
                   "\"max_cycle_us\":%u,\"last_latency_us\":%u,\"max_latency_us\":%u,\"samples\":%llu,"
                   "\"bad_frames\":%llu,\"subscribers\":%u},\"beds\":[",
              (unsigned long long)p->cycles, (unsigned long long)p->overruns, p->period_us, p->last_cycle_us,
              p->max_cycle_us, p->last_latency_us, p->max_latency_us, (unsigned long long)p->samples,
              (unsigned long long)p->bad_frames, p->subscribers);
    int first = 1;
    for (int i = 0; i < VG_MAX_BEDS; i++) {
        const vg_bed_view_t *v = &p->bed[i];
        if (!v->seen) continue;
        sb_printf(&sb, "%s{\"bed\":%d,\"online\":%s,\"age_ms\":%u,\"level\":\"%s\",\"audible\":%s,\"hr\":", first ? "" : ",",
                  i + 1, v->online ? "true" : "false", v->age_ms, vg_level_name(v->level), v->audible ? "true" : "false");
        first = 0;
        jnum(&sb, v, VG_HR, 0); sb_printf(&sb, ",\"spo2\":"); jnum(&sb, v, VG_SPO2, 0);
        sb_printf(&sb, ",\"rr\":"); jnum(&sb, v, VG_RR, 0); sb_printf(&sb, ",\"sbp\":"); jnum(&sb, v, VG_SBP, 0);
        sb_printf(&sb, ",\"temp\":"); jnum(&sb, v, VG_TEMP, 1);
        /* Consciousness and oxygen follow the same rule as the vital signs: expired or never delivered is null. */
        if (v->acvpu_valid) sb_printf(&sb, ",\"acvpu\":\"%c\"", v->acvpu); else sb_printf(&sb, ",\"acvpu\":null");
        if (v->o2_valid) sb_printf(&sb, ",\"o2\":%d,", v->o2); else sb_printf(&sb, ",\"o2\":null,");
        if (v->news2.valid) {
            sb_printf(&sb, "\"news2\":{\"total\":%d,\"band\":\"%s\"},\"news2_missing\":[],", v->news2.total, vg_news2_band(&v->news2));
        } else {
            sb_printf(&sb, "\"news2\":null,\"news2_missing\":[");
            int fm = 1;
            for (int q = 0; q < VG_OBS_COUNT; q++) {
                int ok = q < VG_PARAM_COUNT ? v->valid[q] : (q == VG_OBS_ACVPU ? v->acvpu_valid : v->o2_valid);
                if (!ok) { sb_printf(&sb, "%s\"%s\"", fm ? "" : ",", vg_obs_name(q)); fm = 0; }
            }
            sb_printf(&sb, "],");
        }
        sb_printf(&sb, "\"alarms\":[");
        int f2 = 1;
        for (int s = 0; s < VG_SRC_COUNT; s++) {
            if (v->src_level[s] == VG_LVL_NONE) continue;
            sb_printf(&sb, "%s{\"src\":\"%s\",\"level\":\"%s\"}", f2 ? "" : ",", vg_src_name(s),
                      vg_level_name(v->src_level[s]));
            f2 = 0;
        }
        sb_printf(&sb, "],\"stale\":[");
        f2 = 1;
        for (int q = 0; q < VG_OBS_COUNT; q++) {
            if (v->stale_mask & (1u << q)) { sb_printf(&sb, "%s\"%s\"", f2 ? "" : ",", vg_obs_name(q)); f2 = 0; }
        }
        sb_printf(&sb, "]}");
    }
    sb_printf(&sb, "]}\n");
    return sb.pos;
}
