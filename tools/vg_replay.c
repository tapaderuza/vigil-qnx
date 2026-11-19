/*
 * vg-replay - run the Vigil alarm engine over a recorded or simulated stream of vital-sign
 * frames, in simulated time, and print every alarm transition.
 *
 * Input: one sentence per line, optionally prefixed by a timestamp in milliseconds:
 *     1500 $VGVIT,1,72,98,14,122,36.8,A,0*5C
 * Deterministic: the same input always yields the same transitions, so scenarios are regression
 * tests (sim/test_sim.py). Builds for the host and for QNX 8.0, so the same scenarios run on both.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/core/alarms.h"
#include "../src/core/nmea.h"
#include "../src/core/vitals.h"

static vg_beds_t    g_beds;
static vg_engine_t  g_engine;
static vg_picture_t g_pic;
static vg_events_t  g_events;
static uint64_t     g_samples, g_bad;
static uint64_t     g_frame_every_ms, g_next_frame_ms;

static void usage(void)
{
    fprintf(stderr,
            "usage: vg-replay [-p period_ms] [-t tail_s] [-c 'key value']... [-s pause_at_s] [-j] [-a] [-b] [-f sec] < scenario\n"
            "  -p  engine period in ms (default 100)\n  -t  keep running this many seconds after the last line\n"
            "  -c  alarm setting, e.g. -c 'hr_high_med 110' (repeatable)\n"
            "  -s  start an audio pause at this simulated second\n"
            "  -j  print the final picture as JSON     -a  print the final alarm list     -b  print the bed table\n"
            "  -f  print the picture as one JSON line every N simulated seconds (for the dashboard)\n");
}

static void emit_frame(void)
{
    static char fb[64 * 1024];
    g_pic.samples = g_samples;
    g_pic.bad_frames = g_bad;
    vg_render_json(&g_pic, &g_engine.cfg, fb, sizeof(fb));
    fputs(fb, stdout);
}

static void step(uint64_t now_ms)
{
    vg_transition_t tr[64];
    uint32_t n = vg_engine_step(&g_engine, &g_beds, now_ms, &g_pic, tr, 64);
    if (n > 0) vg_events_push(&g_events, now_ms, tr, n, &g_pic);
    for (uint32_t i = 0; i < n; i++) {
        printf("T+%07.1fs bed %u %-7s %-6s -> %-6s", now_ms / 1000.0, tr[i].bed, vg_src_name(tr[i].src),
               vg_level_name((vg_level_t)tr[i].from), vg_level_name((vg_level_t)tr[i].to));
        const vg_bed_view_t *v = &g_pic.bed[tr[i].bed - 1];
        if (tr[i].src < VG_PARAM_COUNT && v->valid[tr[i].src]) printf("  %s=%.*f", vg_param_name(tr[i].src), tr[i].src == VG_TEMP ? 1 : 0, v->v[tr[i].src]);
        if (tr[i].src == VG_SRC_NEWS2 && v->news2.valid) printf("  score=%d", v->news2.total);
        printf("\n");
    }
    if (g_frame_every_ms != 0 && now_ms >= g_next_frame_ms) {
        emit_frame();
        g_next_frame_ms = now_ms + g_frame_every_ms;
    }
}

static void ingest(const char *line, uint64_t now_ms)
{
    vg_nmea_t s;
    vg_sample_t smp;
    g_samples++;
    if (vg_nmea_parse(line, strlen(line), &s) != VG_NMEA_OK || !vg_sample_parse(&s, &smp)) {
        g_bad++;
        return;
    }
    vg_beds_apply(&g_beds, &smp, now_ms);
}

int main(int argc, char **argv)
{
    vg_alarm_cfg_t cfg;
    vg_alarm_cfg_default(&cfg);
    uint64_t period_ms = 100, tail_ms = 0, pause_at_ms = 0;
    int pause_set = 0, print_json = 0, print_alarms = 0, print_beds = 0, opt;
    char err[128];
    while ((opt = getopt(argc, argv, "p:t:c:s:f:jabh")) != -1) {
        switch (opt) {
        case 'p': period_ms = strtoull(optarg, NULL, 10); break;
        case 't': tail_ms = (uint64_t)(atof(optarg) * 1000.0); break;
        case 'f': g_frame_every_ms = (uint64_t)(atof(optarg) * 1000.0); break;
        case 's': pause_at_ms = (uint64_t)(atof(optarg) * 1000.0); pause_set = 1; break;
        case 'c':
            if (vg_alarm_cfg_apply(&cfg, optarg, err, sizeof(err)) != 0) {
                fprintf(stderr, "vg-replay: %s\n", err);
                return 2;
            }
            break;
        case 'j': print_json = 1; break;
        case 'a': print_alarms = 1; break;
        case 'b': print_beds = 1; break;
        default: usage(); return opt == 'h' ? 0 : 2;
        }
    }
    if (period_ms == 0 || period_ms > 60000) {
        fprintf(stderr, "vg-replay: period must be 1..60000 ms\n");
        return 2;
    }
    vg_beds_init(&g_beds);
    vg_engine_init(&g_engine, &cfg);
    vg_events_init(&g_events);

    char line[512];
    uint64_t t = 0, next_step = 0;
    int paused_done = 0;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        if (isdigit((unsigned char)*p)) {
            char *end = NULL;
            uint64_t ts = strtoull(p, &end, 10);
            if (ts < t) {
                fprintf(stderr, "vg-replay: timestamps must not go backwards (%llu < %llu)\n",
                        (unsigned long long)ts, (unsigned long long)t);
                return 2;
            }
            t = ts;
            p = end;
            while (*p == ' ' || *p == '\t') p++;
        }
        while (next_step <= t) {
            if (pause_set && !paused_done && next_step >= pause_at_ms) {
                vg_engine_pause_audio(&g_engine, next_step);
                paused_done = 1;
            }
            step(next_step);
            next_step += period_ms;
        }
        ingest(p, t);
    }
    uint64_t end_t = t + tail_ms;
    while (next_step <= end_t) {
        if (pause_set && !paused_done && next_step >= pause_at_ms) {
            vg_engine_pause_audio(&g_engine, next_step);
            paused_done = 1;
        }
        step(next_step);
        next_step += period_ms;
    }
    g_pic.samples = g_samples;
    g_pic.bad_frames = g_bad;
    g_pic.period_us = (uint32_t)(period_ms * 1000);
    static char buf[64 * 1024];
    if (print_beds) { vg_render_beds(&g_pic, buf, sizeof(buf)); fputs(buf, stdout); }
    if (print_alarms) { vg_render_alarms(&g_pic, &g_engine.cfg, buf, sizeof(buf)); fputs(buf, stdout); }
    if (print_json) { vg_render_json(&g_pic, &g_engine.cfg, buf, sizeof(buf)); fputs(buf, stdout); }
    fprintf(stderr, "vg-replay: %llu frames (%llu bad)\n", (unsigned long long)g_samples, (unsigned long long)g_bad);
    return 0;
}
