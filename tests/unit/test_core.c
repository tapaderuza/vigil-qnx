/*
 * Vigil - unit tests for the portable core: frame decoding, NEWS2, the alarm engine, events and
 * renderers. Zero dependencies; the same binary is built with gcc on the host and with qcc for
 * QNX 8.0, and executed on both.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/core/alarms.h"
#include "../../src/core/nmea.h"
#include "../../src/core/vitals.h"

static int g_checks, g_failures;
static const char *g_test;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_test, #cond);       \
        }                                                                              \
    } while (0)

#define RUN(fn)                                                                        \
    do {                                                                               \
        g_test = #fn;                                                                  \
        int _before = g_failures;                                                      \
        fn();                                                                          \
        printf("%s %s\n", g_failures == _before ? "ok  " : "FAIL", #fn);               \
    } while (0)

/* Build "$VGVIT,..." with a correct checksum, parse it, and return the sample. */
static int frame(const char *fields, vg_sample_t *out)
{
    char body[128], line[160];
    snprintf(body, sizeof(body), "VGVIT,%s", fields);
    snprintf(line, sizeof(line), "$%s*%02X", body, vg_nmea_xor(body, strlen(body)));
    vg_nmea_t s;
    if (vg_nmea_parse(line, strlen(line), &s) != VG_NMEA_OK) return -1;
    return vg_sample_parse(&s, out);
}

static void test_frame_parse(void)
{
    vg_sample_t s;
    CHECK(frame("3,72,98,14,122,36.8,A,0", &s) == 1);
    CHECK(s.bed == 3 && s.has[VG_HR] && s.v[VG_HR] == 72 && s.v[VG_SPO2] == 98 && s.v[VG_RR] == 14);
    CHECK(s.v[VG_SBP] == 122 && s.v[VG_TEMP] == 36.8 && s.acvpu == 'A' && s.o2 == 0);

    CHECK(frame("1,,91,18,,37.0,V,1", &s) == 1);      /* empty fields: sensors not delivering */
    CHECK(!s.has[VG_HR] && s.has[VG_SPO2] && !s.has[VG_SBP] && s.acvpu == 'V' && s.o2 == 1);

    CHECK(frame("1,500,98,14,122,36.8,A,0", &s) == 1); /* impossible value: treated as a sensor fault */
    CHECK(!s.has[VG_HR] && s.has[VG_SPO2]);
    CHECK(frame("1,72,98,14,122,36.8,X,2", &s) == 1);  /* bad consciousness / oxygen flag */
    CHECK(s.acvpu == 0 && s.o2 == -1);

    CHECK(frame("0,72,98,14,122,36.8,A,0", &s) == 0);  /* bed out of range */
    CHECK(frame("9,72,98,14,122,36.8,A,0", &s) == 0);
    CHECK(frame("1,72,98", &s) == 0);                  /* too few fields */

    vg_nmea_t n;                                       /* corrupted checksum never reaches the engine */
    const char *bad = "$VGVIT,1,72,98,14,122,36.8,A,0*00";
    CHECK(vg_nmea_parse(bad, strlen(bad), &n) == VG_NMEA_ERR_CHECKSUM);
}

static vg_news2_t score(const char *fields)
{
    vg_sample_t s;
    vg_news2_t n;
    memset(&n, 0, sizeof(n));
    if (frame(fields, &s) == 1) vg_news2_compute(s.v, s.has, s.acvpu, s.o2, &n);
    return n;
}

static void test_news2(void)
{
    vg_news2_t n = score("1,72,98,14,122,36.8,A,0");
    CHECK(n.valid && n.total == 0 && !n.any3 && strcmp(vg_news2_band(&n), "LOW") == 0);

    n = score("1,118,92,24,98,38.6,A,1");              /* 2+2+2+2+1+0+2 = 11 */
    CHECK(n.valid && n.total == 11 && strcmp(vg_news2_band(&n), "HIGH") == 0);

    n = score("1,115,94,22,125,37.0,A,0");             /* 2+1+2 = 5 */
    CHECK(n.total == 5 && strcmp(vg_news2_band(&n), "MEDIUM") == 0);

    n = score("1,72,98,25,122,36.8,A,0");              /* a single 3 in one parameter */
    CHECK(n.total == 3 && n.any3 && strcmp(vg_news2_band(&n), "LOW-MEDIUM") == 0);

    n = score("1,72,98,14,122,36.8,V,0");              /* any non-alert state scores 3 */
    CHECK(n.part[5] == 3 && n.total == 3);

    /* Edges of every band, from the published chart. */
    CHECK(score("1,40,98,14,122,36.8,A,0").part[4] == 3 && score("1,41,98,14,122,36.8,A,0").part[4] == 1);
    CHECK(score("1,72,91,14,122,36.8,A,0").part[1] == 3 && score("1,72,96,14,122,36.8,A,0").part[1] == 0);
    CHECK(score("1,72,98,8,122,36.8,A,0").part[0] == 3 && score("1,72,98,9,122,36.8,A,0").part[0] == 1);
    CHECK(score("1,72,98,14,220,36.8,A,0").part[3] == 3 && score("1,72,98,14,219,36.8,A,0").part[3] == 0);
    CHECK(score("1,72,98,14,122,35.0,A,0").part[6] == 3 && score("1,72,98,14,122,39.1,A,0").part[6] == 2);

    n = score("1,,98,14,122,36.8,A,0");                /* a missing parameter: no score, never a guess */
    CHECK(!n.valid && strcmp(vg_news2_band(&n), "N/A") == 0);
}

/* ------------------------------------------------------------------------ engine */

static vg_beds_t   g_beds;
static vg_engine_t g_eng;
static vg_picture_t g_pic;

static void setup(void)
{
    vg_beds_init(&g_beds);
    vg_engine_init(&g_eng, NULL);
    memset(&g_pic, 0, sizeof(g_pic));
}

static void feed(const char *fields, uint64_t t)
{
    vg_sample_t s;
    if (frame(fields, &s) == 1) vg_beds_apply(&g_beds, &s, t);
}

/* Run the engine every 100 ms from `from` to `to`, feeding `fields` once per second. */
static uint32_t run(const char *fields, uint64_t from, uint64_t to, vg_transition_t *tr, uint32_t max)
{
    uint32_t n = 0;
    for (uint64_t t = from; t <= to; t += 100) {
        if (fields != NULL && t % 1000 == 0) feed(fields, t);
        n += vg_engine_step(&g_eng, &g_beds, t, &g_pic, tr + (n < max ? n : max), max > n ? max - n : 0);
    }
    return n;
}

static void test_onset_and_release_delays(void)
{
    setup();
    vg_transition_t tr[16];
    uint32_t n = run("1,72,98,14,122,36.8,A,0", 0, 4000, tr, 16);
    CHECK(n == 0 && g_pic.highest == VG_LVL_NONE);

    /* Heart rate 150 (above the HIGH limit of 140) from t=5000: raised only after the 3 s onset. */
    run("1,150,98,14,122,36.8,A,0", 5000, 7900, tr, 16);
    CHECK(g_pic.bed[0].src_level[VG_HR] == VG_LVL_NONE);
    n = run("1,150,98,14,122,36.8,A,0", 7900, 8200, tr, 16);
    CHECK(g_pic.bed[0].src_level[VG_HR] == VG_LVL_HIGH && g_pic.n_high == 1 && g_pic.bed[0].audible);
    CHECK(n >= 1 && tr[0].bed == 1 && tr[0].src == VG_HR && tr[0].from == VG_LVL_NONE && tr[0].to == VG_LVL_HIGH);

    /* Back to normal at t=9000: the alarm is lowered only after the 5 s release delay. */
    run("1,72,98,14,122,36.8,A,0", 8300, 13800, tr, 16);
    CHECK(g_pic.bed[0].src_level[VG_HR] == VG_LVL_HIGH);
    run("1,72,98,14,122,36.8,A,0", 13900, 14500, tr, 16);
    CHECK(g_pic.bed[0].src_level[VG_HR] == VG_LVL_NONE);
    /* The aggregate score (HR 150 alone scores 3) is released more slowly: 30 s, not 5 s. */
    CHECK(g_pic.bed[0].src_level[VG_SRC_NEWS2] == VG_LVL_LOW && g_pic.highest == VG_LVL_LOW);
    run("1,72,98,14,122,36.8,A,0", 14600, 40000, tr, 16);
    CHECK(g_pic.highest == VG_LVL_NONE);
}

static void test_value_hysteresis_stops_flicker(void)
{
    setup();
    vg_transition_t tr[64];
    run("1,125,98,14,122,36.8,A,0", 0, 4000, tr, 16);       /* HR 125: MEDIUM raised */
    CHECK(g_pic.bed[0].src_level[VG_HR] == VG_LVL_MEDIUM);
    /* The value hovers on the limit (120 / 119): inside by less than the 3 bpm margin, so it never clears. */
    uint32_t n = 0;
    for (uint64_t t = 4000; t < 30000; t += 1000) {
        feed(t % 2000 == 0 ? "1,119,98,14,122,36.8,A,0" : "1,121,98,14,122,36.8,A,0", t);
        for (int k = 0; k < 10; k++) n += vg_engine_step(&g_eng, &g_beds, t + (uint64_t)k * 100, &g_pic, tr, 64);
    }
    CHECK(g_pic.bed[0].src_level[VG_HR] == VG_LVL_MEDIUM);
    /* A genuine recovery (well inside) does clear it. */
    run("1,90,98,14,122,36.8,A,0", 30000, 40000, tr, 16);
    CHECK(g_pic.bed[0].src_level[VG_HR] == VG_LVL_NONE);
    (void)n;
}

static void test_noise_never_alarms(void)
{
    setup();
    vg_transition_t tr[16];
    uint32_t total = 0;
    /* One high-HR sample every 4 seconds between normal ones never persists for 3 s. */
    for (uint64_t t = 0; t < 40000; t += 1000) {
        feed(t % 4000 == 0 ? "1,150,98,14,122,36.8,A,0" : "1,72,98,14,122,36.8,A,0", t);
        for (int k = 0; k < 10; k++) total += vg_engine_step(&g_eng, &g_beds, t + (uint64_t)k * 100, &g_pic, tr, 16);
    }
    CHECK(total == 0);
}

static void test_priorities(void)
{
    setup();
    vg_transition_t tr[16];
    run("1,125,98,14,122,36.8,A,0", 0, 4000, tr, 16);       /* 125 bpm: above 120 -> MEDIUM */
    CHECK(g_pic.bed[0].src_level[VG_HR] == VG_LVL_MEDIUM);
    run("1,125,89,14,122,36.8,A,0", 4100, 8000, tr, 16);    /* SpO2 89: below 92 -> MEDIUM, not HIGH */
    CHECK(g_pic.bed[0].src_level[VG_SPO2] == VG_LVL_MEDIUM);
    run("1,125,84,14,122,36.8,A,0", 8100, 12000, tr, 16);   /* SpO2 84: below 88 -> HIGH */
    CHECK(g_pic.bed[0].src_level[VG_SPO2] == VG_LVL_HIGH && g_pic.bed[0].level == VG_LVL_HIGH);
}

static void test_sensor_off_is_a_technical_alarm_not_a_stale_number(void)
{
    setup();
    vg_transition_t tr[32];
    run("1,72,80,14,122,36.8,A,0", 0, 4000, tr, 32);        /* SpO2 80: HIGH once the onset has passed */
    CHECK(g_pic.bed[0].src_level[VG_SPO2] == VG_LVL_HIGH);

    /* The probe comes off: frames keep arriving, but with the SpO2 field empty. */
    run("1,72,,14,122,36.8,A,0", 4100, 9500, tr, 32);   /* last SpO2 at 4000: older than 5 s by now */
    CHECK(g_pic.bed[0].valid[VG_SPO2] == 0);
    CHECK(g_pic.bed[0].src_level[VG_SPO2] == VG_LVL_NONE);  /* the stale 80 is no longer shown as live */
    CHECK(g_pic.bed[0].src_level[VG_SRC_SENSOR] == VG_LVL_MEDIUM);
    CHECK(g_pic.bed[0].stale_mask == (1u << VG_SPO2));
    CHECK(g_pic.bed[0].valid[VG_HR] == 1);                   /* the other values are still judged */

    char buf[1024];
    vg_render_alarms(&g_pic, &g_eng.cfg, buf, sizeof(buf));
    CHECK(strstr(buf, "TECHNICAL sensor not delivering: SpO2") != NULL);

    run("1,72,97,14,122,36.8,A,0", 9100, 10000, tr, 32);     /* probe back on */
    CHECK(g_pic.bed[0].valid[VG_SPO2] == 1 && g_pic.bed[0].src_level[VG_SRC_SENSOR] == VG_LVL_NONE);
}

static void test_never_reported_parameter_is_not_a_fault(void)
{
    setup();
    vg_transition_t tr[8];
    uint32_t n = run("2,72,98,,,,A,0", 0, 20000, tr, 8);     /* a bed with only HR and SpO2 attached */
    CHECK(n == 0 && g_pic.bed[1].stale_mask == 0 && !g_pic.bed[1].news2.valid);
}

static void test_offline_bed(void)
{
    setup();
    vg_transition_t tr[16];
    run("4,72,98,14,122,36.8,A,0", 0, 3000, tr, 16);
    CHECK(g_pic.bed[3].online);
    run(NULL, 3100, 12000, tr, 16);                          /* 9 s of silence: not yet offline */
    CHECK(g_pic.bed[3].online);
    run(NULL, 12100, 14000, tr, 16);
    CHECK(!g_pic.bed[3].online && g_pic.bed[3].src_level[VG_SRC_OFFLINE] == VG_LVL_MEDIUM);
    CHECK(g_pic.bed[3].valid[VG_HR] == 0);                   /* nothing is judged on a silent bed */
    feed("4,72,98,14,122,36.8,A,0", 14500);                  /* it comes back */
    run(NULL, 14600, 14800, tr, 16);
    CHECK(g_pic.bed[3].online && g_pic.bed[3].src_level[VG_SRC_OFFLINE] == VG_LVL_NONE);
}

static void test_news2_raises_alarm_even_when_each_value_is_inside_limits(void)
{
    setup();
    vg_transition_t tr[16];
    /* Every value is inside its own limits, but together they score 5 (MEDIUM): HR 115, SpO2 94, RR 22. */
    run("5,115,94,22,125,37.0,A,0", 0, 4500, tr, 16);
    CHECK(g_pic.bed[4].src_level[VG_HR] == VG_LVL_NONE && g_pic.bed[4].src_level[VG_RR] == VG_LVL_NONE);
    CHECK(g_pic.bed[4].news2.total == 5 && g_pic.bed[4].src_level[VG_SRC_NEWS2] == VG_LVL_MEDIUM);
    CHECK(g_pic.n_medium == 1);
}

static void test_audio_pause_never_hides_the_alarm(void)
{
    setup();
    vg_transition_t tr[16];
    run("1,150,98,14,122,36.8,A,0", 0, 4000, tr, 16);
    CHECK(g_pic.bed[0].audible && !g_pic.audio_paused);
    vg_engine_pause_audio(&g_eng, 4000);
    run("1,150,98,14,122,36.8,A,0", 4100, 4200, tr, 16);
    CHECK(g_pic.audio_paused && g_pic.pause_remaining_s > 100);
    CHECK(!g_pic.bed[0].audible && g_pic.bed[0].level == VG_LVL_HIGH); /* silent, but still shown HIGH */
    run("1,150,98,14,122,36.8,A,0", 4300, 125000, tr, 16);
    CHECK(!g_pic.audio_paused && g_pic.bed[0].audible);                /* the pause expired by itself */
    vg_engine_pause_audio(&g_eng, 125000);
    vg_engine_resume_audio(&g_eng);
    run("1,150,98,14,122,36.8,A,0", 125100, 125200, tr, 16);
    CHECK(!g_pic.audio_paused);
}

static void test_config(void)
{
    vg_alarm_cfg_t c;
    char err[96];
    vg_alarm_cfg_default(&c);
    CHECK(vg_alarm_cfg_apply(&c, "hr_high_med 110", err, sizeof(err)) == 0 && c.lim[VG_HR].high_med == 110);
    CHECK(vg_alarm_cfg_apply(&c, "spo2_low_high 85", err, sizeof(err)) == 0 && c.lim[VG_SPO2].low_high == 85);
    CHECK(vg_alarm_cfg_apply(&c, "onset_s 2", err, sizeof(err)) == 0 && c.onset_ms == 2000);
    CHECK(vg_alarm_cfg_apply(&c, "hr_high_med 200", err, sizeof(err)) == -1);   /* above hr_high_high */
    CHECK(strstr(err, "limits must satisfy") != NULL);
    CHECK(vg_alarm_cfg_apply(&c, "sensor_s 30", err, sizeof(err)) == -1);        /* offline must be >= sensor */
    CHECK(vg_alarm_cfg_apply(&c, "bogus 1", err, sizeof(err)) == -1 && strstr(err, "unknown") != NULL);
    CHECK(vg_alarm_cfg_apply(&c, "hr_high_med -3", err, sizeof(err)) == -1);
    CHECK(vg_alarm_cfg_apply(&c, "pause_s 9999", err, sizeof(err)) == -1);

    char buf[1024];
    vg_alarm_cfg_t d = c;
    vg_render_config(&c, buf, sizeof(buf));
    char *save = NULL;
    for (char *l = strtok_r(buf, "\n", &save); l != NULL; l = strtok_r(NULL, "\n", &save)) {
        CHECK(vg_alarm_cfg_apply(&d, l, err, sizeof(err)) == 0);                /* every rendered line is accepted back */
    }
    CHECK(d.lim[VG_HR].high_med == c.lim[VG_HR].high_med && d.onset_ms == c.onset_ms);
}

static void test_event_ring(void)
{
    vg_events_t r;
    vg_events_init(&r);
    vg_event_t out[VG_EVENT_RING];
    uint32_t lost = 9;
    CHECK(vg_events_since(&r, 0, out, VG_EVENT_RING, &lost) == 0 && lost == 0);

    setup();
    vg_transition_t tr[16];
    run("1,150,98,14,122,36.8,A,0", 0, 4000, tr, 16);
    vg_transition_t t = { 1, VG_HR, VG_LVL_NONE, VG_LVL_HIGH };
    vg_events_push(&r, 3000, &t, 1, &g_pic);
    CHECK(vg_events_since(&r, 0, out, VG_EVENT_RING, &lost) == 1 && out[0].value_ok && out[0].value == 150.0f);
    for (int i = 0; i < VG_EVENT_RING + 10; i++) vg_events_push(&r, 5000 + (uint64_t)i, &t, 1, &g_pic);
    uint32_t n = vg_events_since(&r, 1, out, VG_EVENT_RING, &lost);
    CHECK(n == VG_EVENT_RING && lost == 10 && out[0].seq == 12 && out[n - 1].seq == 139);
    CHECK(vg_events_since(&r, VG_EVENTS_FROM_NOW, out, VG_EVENT_RING, &lost) == 0 && lost == 0);
    char buf[256];
    vg_render_events(out, 1, buf, sizeof(buf));
    CHECK(strstr(buf, "bed 1 HR") != NULL && strstr(buf, "NONE -> HIGH") != NULL && strstr(buf, "value 150") != NULL);
}

static void test_renderers_and_json(void)
{
    setup();
    vg_transition_t tr[32];
    feed("1,72,98,14,122,36.8,A,0", 0);
    feed("2,150,84,26,85,39.5,V,1", 0);
    feed("3,72,,14,122,36.8,A,0", 0);
    run(NULL, 0, 4000, tr, 32);
    char buf[4096];
    vg_render_beds(&g_pic, buf, sizeof(buf));
    CHECK(strstr(buf, "BED") != NULL && strstr(buf, "HIGH") != NULL);
    vg_render_alarms(&g_pic, &g_eng.cfg, buf, sizeof(buf));
    CHECK(strstr(buf, "bed 2") != NULL && strstr(buf, "SpO2 84 % too low") != NULL);

    static char js[16384];
    size_t n = vg_render_json(&g_pic, &g_eng.cfg, js, sizeof(js));
    CHECK(n == strlen(js) && n > 200 && js[n - 2] == '}' && js[n - 1] == '\n');
    int depth = 0, in_str = 0;
    for (size_t i = 0; i < n; i++) {
        char c = js[i];
        if (in_str) { if (c == '\\') i++; else if (c == '"') in_str = 0; }
        else if (c == '"') in_str = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
    }
    CHECK(depth == 0 && !in_str);
    CHECK(strstr(js, "\"spo2\":null") != NULL);                         /* a missing sensor is null, never 0 */

    char tiny[24];
    n = vg_render_json(&g_pic, &g_eng.cfg, tiny, sizeof(tiny));
    CHECK(n == sizeof(tiny) - 1 && tiny[sizeof(tiny) - 1] == '\0');
}

int main(void)
{
    printf("vigil core unit tests\n");
    RUN(test_frame_parse);
    RUN(test_news2);
    RUN(test_onset_and_release_delays);
    RUN(test_noise_never_alarms);
    RUN(test_value_hysteresis_stops_flicker);
    RUN(test_priorities);
    RUN(test_sensor_off_is_a_technical_alarm_not_a_stale_number);
    RUN(test_never_reported_parameter_is_not_a_fault);
    RUN(test_offline_bed);
    RUN(test_news2_raises_alarm_even_when_each_value_is_inside_limits);
    RUN(test_audio_pause_never_hides_the_alarm);
    RUN(test_config);
    RUN(test_event_ring);
    RUN(test_renderers_and_json);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
