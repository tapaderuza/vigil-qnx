/*
 * vigild - real-time patient-monitor alarm daemon for QNX 8.0.
 *
 * The main thread becomes the *engine*: a SCHED_FIFO thread that
 *   - owns every piece of mutable state (beds, alarm state, events),
 *   - wakes on a periodic timer *pulse* (default 10 Hz) to evaluate every alarm condition,
 *   - receives decoded vital-sign frames from the ingest thread, and queries from vigil-ctl and
 *     vigil-watch, on one named channel,
 *   - tells subscribers about alarm changes with MsgDeliverEvent().
 *
 * No locks protect the engine state because nothing else touches it; the microkernel's
 * synchronous message passing is the synchronisation.
 *
 * Vigil is a software exercise, not a medical device.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <time.h>

#include "vigild.h"

#define VG_PULSE_TICK (_PULSE_CODE_MINAVAIL + 1)
#define VG_PULSE_QUIT (_PULSE_CODE_MINAVAIL + 2)

vgd_options_t  g_opt;
int            g_engine_chid = -1;
vgd_counters_t g_cnt;
slog2_buffer_t g_slog;

/* ------------------------------------------------------------- publishing */

static pthread_mutex_t g_pub_lock;
static vgd_pub_t       g_pub;

void vgd_pub_snapshot(vgd_pub_t *dst)
{
    pthread_mutex_lock(&g_pub_lock);
    memcpy(dst, &g_pub, sizeof(*dst));
    pthread_mutex_unlock(&g_pub_lock);
}

/* ---------------------------------------------------------------- helpers */

uint64_t vgd_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void vgd_log(int severity, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_slog != NULL) {
        slog2f(g_slog, 0, (uint8_t)severity, "%s", line);
    }
    if (g_opt.foreground || g_slog == NULL) {
        fprintf(stderr, "vigild: %s\n", line);
    }
}

int vgd_spawn(pthread_t *t, int prio, const char *name, void *(*fn)(void *))
{
    pthread_attr_t attr;
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);
    int rc = pthread_create(t, &attr, fn, NULL);
    if (rc != 0) {
        vgd_log(SLOG2_WARNING, "cannot start %s at priority %d (%s); using inherited priority", name, prio,
                strerror(rc));
        pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
        rc = pthread_create(t, &attr, fn, NULL);
    }
    pthread_attr_destroy(&attr);
    if (rc == 0) {
        pthread_setname_np(*t, name);
    } else {
        vgd_log(SLOG2_ERROR, "cannot start %s: %s", name, strerror(rc));
    }
    return rc;
}

/* ----------------------------------------------------------- engine state */

static vg_beds_t    g_beds;
static vg_engine_t  g_engine;
static vg_picture_t g_pic;
static vg_events_t  g_events;

static struct {
    int             active;
    rcvid_t         rcvid;
    int             scoid;
    struct sigevent ev;
} g_subs[VG_MAX_SUBSCRIBERS];

static uint64_t g_cycles, g_overruns;
static uint32_t g_last_cycle_us, g_max_cycle_us, g_last_latency_us, g_max_latency_us;
static char     g_text[VG_TEXT_MAX];

static void publish(void)
{
    pthread_mutex_lock(&g_pub_lock);
    g_pub.pic = g_pic;
    g_pub.cfg = g_engine.cfg;
    g_pub.events = g_events;
    pthread_mutex_unlock(&g_pub_lock);
}

static uint32_t count_subs(void)
{
    uint32_t n = 0;
    for (int i = 0; i < VG_MAX_SUBSCRIBERS; i++) {
        n += g_subs[i].active != 0;
    }
    return n;
}

static void notify_subscribers(void)
{
    for (int i = 0; i < VG_MAX_SUBSCRIBERS; i++) {
        if (g_subs[i].active && MsgDeliverEvent(g_subs[i].rcvid, &g_subs[i].ev) == -1) {
            vgd_log(SLOG2_INFO, "dropping subscriber (scoid %d): %s", g_subs[i].scoid, strerror(errno));
            g_subs[i].active = 0;
        }
    }
}

static void fill_health(uint32_t period_us)
{
    g_pic.cycles = g_cycles;
    g_pic.overruns = g_overruns;
    g_pic.period_us = period_us;
    g_pic.last_cycle_us = g_last_cycle_us;
    g_pic.max_cycle_us = g_max_cycle_us;
    g_pic.last_latency_us = g_last_latency_us;
    g_pic.max_latency_us = g_max_latency_us;
    g_pic.samples = atomic_load(&g_cnt.samples);
    g_pic.bad_frames = atomic_load(&g_cnt.bad_frames);
    g_pic.subscribers = count_subs();
}

/* One evaluation cycle, driven by the timer pulse. */
static void on_tick(uint64_t tick_no, uint64_t period_ns, uint64_t t_start_ns)
{
    const uint64_t wake_ns = vgd_now_ns();
    const uint64_t due_ns = t_start_ns + tick_no * period_ns;
    const uint64_t latency_us = wake_ns > due_ns ? (wake_ns - due_ns) / 1000u : 0u;
    const uint64_t now_ms = wake_ns / 1000000u;

    vg_transition_t tr[VG_MAX_TRANSITIONS];
    uint32_t ntr = vg_engine_step(&g_engine, &g_beds, now_ms, &g_pic, tr, VG_MAX_TRANSITIONS);

    const uint64_t done_ns = vgd_now_ns();
    const uint32_t cycle_us = (uint32_t)((done_ns - wake_ns) / 1000u);
    g_cycles++;
    g_last_cycle_us = cycle_us;
    g_last_latency_us = (uint32_t)(latency_us > 0xffffffffu ? 0xffffffffu : latency_us);
    if (cycle_us > g_max_cycle_us) {
        g_max_cycle_us = cycle_us;
    }
    if (g_last_latency_us > g_max_latency_us) {
        g_max_latency_us = g_last_latency_us;
    }
    /*
     * g_overruns counts engine wakes that start more than one whole period after their timer expiry. It says
     * how late the engine thread gets the CPU. It does NOT measure when an alarm reaches anyone: publishing
     * and notifying happen after this point (see the delivery trace in docs/DELIVERY.md). Exposed as
     * deadline_overruns in /dev/vigil/stats and shown as "late engine wakes" on the dashboard.
     */
    if (latency_us > period_ns / 1000u) {
        g_overruns++;
    }

    fill_health((uint32_t)(period_ns / 1000u));
    const uint64_t first_seq = g_events.next_seq;
    if (ntr > 0) {
        vg_events_push(&g_events, now_ms, tr, ntr, &g_pic);
        /*
         * Delivery trace, point D: done_ns was read right after vg_engine_step() returned, i.e. the moment
         * the transitions were decided. Everything after it (logging, publishing, notifying, the consumer
         * waking up) is what the delivery measurement is about. See docs/DELIVERY.md.
         */
        vg_events_stamp(&g_events, first_seq, ntr, done_ns, 0);
        for (uint32_t i = 0; i < ntr; i++) {
            vgd_log(SLOG2_NOTICE, "alarm bed %u %s %s -> %s", tr[i].bed, vg_src_name(tr[i].src),
                    vg_level_name((vg_level_t)tr[i].from), vg_level_name((vg_level_t)tr[i].to));
        }
    }
    publish();
    if (ntr > 0) {
        notify_subscribers();
        /* Point N: every subscriber's pulse has been delivered. Safe to stamp now: only this thread reads g_events. */
        vg_events_stamp(&g_events, first_seq, ntr, 0, vgd_now_ns());
    }
}

/* -------------------------------------------------------- message handling */

static int require(rcvid_t rcvid, const struct _msg_info *info, size_t want)
{
    if ((size_t)info->msglen < want) {
        MsgError(rcvid, EBADMSG);
        return 0;
    }
    return 1;
}

static void reply_text(rcvid_t rcvid, long status, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void reply_text(rcvid_t rcvid, long status, const char *fmt, ...)
{
    char out[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    MsgReply(rcvid, status, out, (size_t)(n > 0 ? n : 0) + 1);
}

static void handle_config(rcvid_t rcvid, const vg_msg_config_t *m)
{
    char line[VG_CFG_LINE];
    memcpy(line, m->line, sizeof(line));
    line[sizeof(line) - 1] = '\0';
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
        line[--n] = '\0';
    }
    const uint64_t now_ms = vgd_now_ns() / 1000000u;
    if (strncmp(line, "pause", 5) == 0) {
        vg_engine_pause_audio(&g_engine, now_ms);
        vgd_log(SLOG2_NOTICE, "audio paused for %llu s", (unsigned long long)(g_engine.cfg.pause_ms / 1000u));
        reply_text(rcvid, 0, "ok\n");
    } else if (strncmp(line, "resume", 6) == 0) {
        vg_engine_resume_audio(&g_engine);
        vgd_log(SLOG2_NOTICE, "audio resumed");
        reply_text(rcvid, 0, "ok\n");
    } else if (strncmp(line, "reset_stats", 11) == 0) {
        g_max_cycle_us = 0;
        g_max_latency_us = 0;
        g_overruns = 0;
        vgd_log(SLOG2_INFO, "timing statistics reset");
        reply_text(rcvid, 0, "ok\n");
    } else {
        char err[96];
        if (vg_alarm_cfg_apply(&g_engine.cfg, line, err, sizeof(err)) == 0) {
            vgd_log(SLOG2_INFO, "config: %s", line);
            publish(); /* readers of /dev/vigil/config must see the new limit at once, not on the next tick */
            reply_text(rcvid, 0, "ok\n");
        } else {
            vgd_log(SLOG2_WARNING, "config rejected (%s): %s", line, err);
            reply_text(rcvid, 1, "rejected: %s\n", err);
        }
    }
}

static void handle_get_text(rcvid_t rcvid, uint32_t kind)
{
    size_t len = 0;
    switch (kind) {
    case VG_TEXT_STATS: len = vg_render_stats(&g_pic, g_text, sizeof(g_text)); break;
    case VG_TEXT_ALARMS: len = vg_render_alarms(&g_pic, &g_engine.cfg, g_text, sizeof(g_text)); break;
    case VG_TEXT_BEDS: len = vg_render_beds(&g_pic, g_text, sizeof(g_text)); break;
    case VG_TEXT_CONFIG: len = vg_render_config(&g_engine.cfg, g_text, sizeof(g_text)); break;
    default:
        MsgError(rcvid, EINVAL);
        return;
    }
    MsgReply(rcvid, (long)len, g_text, len);
}

static void handle_get_events(rcvid_t rcvid, uint64_t since)
{
    static vg_events_reply_t rep;
    rep.n = vg_events_since(&g_events, since, rep.ev, VG_EVENTS_MAX, &rep.lost);
    rep.next_seq = rep.n > 0 ? rep.ev[rep.n - 1].seq : g_events.next_seq - 1;
    MsgReply(rcvid, EOK, &rep, offsetof(vg_events_reply_t, ev) + rep.n * sizeof(vg_event_t));
}

static void handle_subscribe(rcvid_t rcvid, const struct _msg_info *info, const vg_msg_subscribe_t *m)
{
    int slot = -1;
    for (int i = 0; i < VG_MAX_SUBSCRIBERS; i++) {
        if (g_subs[i].active && g_subs[i].scoid == info->scoid) {
            slot = i; /* one subscription per client process: replace it */
            break;
        }
        if (!g_subs[i].active && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        MsgError(rcvid, EAGAIN);
        return;
    }
    g_subs[slot].active = 1;
    g_subs[slot].rcvid = rcvid; /* stays valid for MsgDeliverEvent() after we reply */
    g_subs[slot].scoid = info->scoid;
    g_subs[slot].ev = m->ev;
    MsgReply(rcvid, EOK, NULL, 0);
    vgd_log(SLOG2_INFO, "subscriber added (scoid %d)", info->scoid);
}

static void handle_message(rcvid_t rcvid, const vg_msg_any_t *m, const struct _msg_info *info)
{
    switch (m->type) {
    case _IO_CONNECT:
        MsgReply(rcvid, EOK, NULL, 0); /* name_open() may probe with a connect message */
        return;

    case VG_MSG_SAMPLE:
        if (require(rcvid, info, sizeof(vg_msg_sample_t))) {
            vg_beds_apply(&g_beds, &m->sample.sample, vgd_now_ns() / 1000000u);
            MsgReply(rcvid, EOK, NULL, 0);
        }
        return;

    case VG_MSG_SUBSCRIBE:
        if (require(rcvid, info, sizeof(vg_msg_subscribe_t))) {
            handle_subscribe(rcvid, info, &m->sub);
        }
        return;

    case VG_MSG_UNSUBSCRIBE:
        for (int i = 0; i < VG_MAX_SUBSCRIBERS; i++) {
            if (g_subs[i].active && g_subs[i].scoid == info->scoid) {
                g_subs[i].active = 0;
            }
        }
        MsgReply(rcvid, EOK, NULL, 0);
        return;

    case VG_MSG_GET_EVENTS:
        if (require(rcvid, info, sizeof(vg_msg_get_events_t))) {
            handle_get_events(rcvid, m->get_events.since_seq);
        }
        return;

    case VG_MSG_GET_TEXT:
        if (require(rcvid, info, sizeof(vg_msg_get_text_t))) {
            handle_get_text(rcvid, m->get_text.kind);
        }
        return;

    case VG_MSG_CONFIG:
        if (require(rcvid, info, sizeof(vg_msg_config_t))) {
            handle_config(rcvid, &m->config);
        }
        return;

    default:
        MsgError(rcvid, ENOSYS); /* includes every other system (_IO_*) message */
        return;
    }
}

/* -------------------------------------------------------------- main loop */

static void on_quit_signal(int sig)
{
    (void)sig;
    MsgSendPulse(-g_engine_chid, -1, VG_PULSE_QUIT, 0); /* negative coid = local channel */
}

static int set_self_priority(int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

static void engine_loop(name_attach_t *attach, uint64_t period_ns, uint64_t t_start_ns)
{
    union {
        vg_msg_any_t  msg;
        struct _pulse pulse;
    } buf;
    uint64_t tick_no = 0;

    for (;;) {
        struct _msg_info info;
        rcvid_t rcvid = MsgReceive(attach->chid, &buf, sizeof(buf), &info);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            vgd_log(SLOG2_ERROR, "MsgReceive failed: %s", strerror(errno));
            return;
        }
        if (rcvid == 0) {
            switch (buf.pulse.code) {
            case VG_PULSE_TICK:
                on_tick(++tick_no, period_ns, t_start_ns);
                break;
            case VG_PULSE_QUIT:
                vgd_log(SLOG2_NOTICE, "shutting down");
                return;
            case _PULSE_CODE_DISCONNECT:
                for (int i = 0; i < VG_MAX_SUBSCRIBERS; i++) {
                    if (g_subs[i].active && g_subs[i].scoid == buf.pulse.scoid) {
                        g_subs[i].active = 0;
                    }
                }
                ConnectDetach(buf.pulse.scoid);
                break;
            default:
                break;
            }
            continue;
        }
        handle_message(rcvid, &buf.msg, &info);
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -u PORT   UDP port for vital-sign frames (default 10120)\n"
            "  -g ADDR   also join this IPv4 multicast group\n"
            "  -w PORT   HTTP dashboard port, 0 = off (default 8080)\n"
            "  -b ADDR   HTTP bind address (default 0.0.0.0; use 127.0.0.1 to keep it local)\n"
            "  -p MS     engine period in milliseconds, 10..1000 (default 100)\n"
            "  -E/-R/-I/-H PRIO  priorities of engine/resmgr/ingest/http (default 40/25/20/10)\n"
            "  -d PATH   resource manager prefix (default /dev/vigil)\n"
            "  -f        also log to stderr\n",
            argv0);
}

int main(int argc, char **argv)
{
    g_opt.udp_port = 10120;
    g_opt.http_port = 8080;
    snprintf(g_opt.http_bind, sizeof(g_opt.http_bind), "0.0.0.0");
    g_opt.period_ms = 100;
    g_opt.prio_engine = 40;
    g_opt.prio_resmgr = 25;
    g_opt.prio_ingest = 20;
    g_opt.prio_http = 10;
    snprintf(g_opt.dev_prefix, sizeof(g_opt.dev_prefix), "/dev/vigil");

    int opt;
    while ((opt = getopt(argc, argv, "u:g:w:b:p:E:R:I:H:d:fLh")) != -1) {
        switch (opt) {
        case 'u': g_opt.udp_port = atoi(optarg); break;
        case 'g': snprintf(g_opt.mcast_group, sizeof(g_opt.mcast_group), "%s", optarg); break;
        case 'w': g_opt.http_port = atoi(optarg); break;
        case 'b': snprintf(g_opt.http_bind, sizeof(g_opt.http_bind), "%s", optarg); break;
        case 'p': g_opt.period_ms = (unsigned)atoi(optarg); break;
        case 'E': g_opt.prio_engine = atoi(optarg); break;
        case 'R': g_opt.prio_resmgr = atoi(optarg); break;
        case 'I': g_opt.prio_ingest = atoi(optarg); break;
        case 'H': g_opt.prio_http = atoi(optarg); break;
        case 'd': snprintf(g_opt.dev_prefix, sizeof(g_opt.dev_prefix), "%s", optarg); break;
        case 'f': g_opt.foreground = 1; break;
        case 'L': g_opt.no_lockdown = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (g_opt.period_ms < 10 || g_opt.period_ms > 1000 || g_opt.udp_port <= 0 || g_opt.udp_port > 65535 ||
        g_opt.http_port < 0 || g_opt.http_port > 65535) {
        usage(argv[0]);
        return 2;
    }

    slog2_buffer_set_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.buffer_set_name = "vigil";
    cfg.num_buffers = 1;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "main";
    cfg.buffer_config[0].num_pages = 8;
    if (slog2_register(&cfg, &g_slog, 0) == -1) {
        g_slog = NULL; /* fall back to stderr */
    }

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&g_pub_lock, &ma);
    pthread_mutexattr_destroy(&ma);

    vg_beds_init(&g_beds);
    vg_alarm_cfg_t cfg_default;
    vg_alarm_cfg_default(&cfg_default);
    vg_engine_init(&g_engine, &cfg_default);
    vg_events_init(&g_events);
    memset(&g_pic, 0, sizeof(g_pic));

    signal(SIGPIPE, SIG_IGN);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        vgd_log(SLOG2_WARNING, "mlockall failed (%s): page faults may add latency", strerror(errno));
    }

    name_attach_t *attach = name_attach(NULL, VG_SERVICE_NAME, 0);
    if (attach == NULL) {
        vgd_log(SLOG2_CRITICAL, "name_attach(\"%s\") failed: %s (already running?)", VG_SERVICE_NAME, strerror(errno));
        return 1;
    }
    g_engine_chid = attach->chid;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_quit_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    pthread_t t_res, t_ing, t_http;
    vgd_spawn(&t_res, g_opt.prio_resmgr, "vg-resmgr", vgd_resmgr_thread);
    vgd_spawn(&t_ing, g_opt.prio_ingest, "vg-ingest", vgd_ingest_thread);
    if (g_opt.http_port > 0) {
        vgd_spawn(&t_http, g_opt.prio_http, "vg-http", vgd_http_thread);
    }

    if (set_self_priority(g_opt.prio_engine) != 0) {
        vgd_log(SLOG2_WARNING, "cannot run the engine at SCHED_FIFO priority %d: %s", g_opt.prio_engine, strerror(errno));
    }
    pthread_setname_np(pthread_self(), "vg-engine");

    /* Periodic timer: each expiry delivers a pulse to our own channel. */
    int self_coid = ConnectAttach(0, 0, g_engine_chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, self_coid, g_opt.prio_engine, VG_PULSE_TICK, 0);
    timer_t tid;
    if (self_coid == -1 || timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) {
        vgd_log(SLOG2_CRITICAL, "cannot create the engine timer: %s", strerror(errno));
        return 1;
    }
    struct itimerspec its;
    its.it_value.tv_sec = g_opt.period_ms / 1000u;
    its.it_value.tv_nsec = (long)(g_opt.period_ms % 1000u) * 1000000L;
    its.it_interval = its.it_value;
    const uint64_t period_ns = (uint64_t)g_opt.period_ms * 1000000ull;
    const uint64_t t_start_ns = vgd_now_ns();
    if (timer_settime(tid, 0, &its, NULL) == -1) {
        vgd_log(SLOG2_CRITICAL, "timer_settime failed: %s", strerror(errno));
        return 1;
    }

    vgd_log(SLOG2_NOTICE, "started: udp %d, http %d, period %u ms, engine priority %d, devices %s", g_opt.udp_port,
            g_opt.http_port, g_opt.period_ms, g_opt.prio_engine, g_opt.dev_prefix);
    /* Everything the engine needs is held now: give up every other ability for good (-L keeps them, for debugging). */
    if (!g_opt.no_lockdown) {
        vgd_lockdown(2 + (g_opt.http_port > 0 ? 1 : 0));
    }
    engine_loop(attach, period_ns, t_start_ns);

    timer_delete(tid);
    name_detach(attach, 0);
    return 0;
}
