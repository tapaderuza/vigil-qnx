/*
 * vigil-watch - live alarm feed driven by kernel pulses, not polling.
 *
 * Subscribes to vigild with a *secure event*: it creates its own channel,
 * builds a SIGEV_PULSE event aimed at it, registers that event with
 * MsgRegisterEvent() against the daemon's connection (so only the daemon can
 * fire it), and sends the daemon the event. Then it simply blocks in
 * MsgReceive() at zero CPU until the daemon calls MsgDeliverEvent() because an
 * alarm changed level, fetches the new events and prints them.
 *
 *   vigil-watch            run until Ctrl-C
 *   vigil-watch -n 3       exit after printing 3 events (handy in scripts)
 *   vigil-watch -t 60      exit after 60 s without an event
 *   vigil-watch -a         also print the events already in the history
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>

#include "../src/qnx/vigil_ipc.h"

#define VG_WATCH_PULSE_CODE (_PULSE_CODE_MINAVAIL + 7)

static const char *colour(vg_level_t l)
{
    if (!isatty(STDOUT_FILENO)) {
        return "";
    }
    switch (l) {
    case VG_LVL_HIGH: return "\x1b[1;31m";
    case VG_LVL_MEDIUM: return "\x1b[1;33m";
    case VG_LVL_LOW: return "\x1b[36m";
    default: return "\x1b[32m";
    }
}

static const char *reset(void)
{
    return isatty(STDOUT_FILENO) ? "\x1b[0m" : "";
}

/* Fetch and print everything after *since. Returns the number of events printed. */
static unsigned drain(int srv, uint64_t *since)
{
    static vg_events_reply_t rep;
    unsigned printed = 0;
    for (;;) {
        vg_msg_get_events_t m;
        memset(&m, 0, sizeof(m));
        m.h.type = VG_MSG_GET_EVENTS;
        m.since_seq = *since;
        if (MsgSend(srv, &m, sizeof(m), &rep, sizeof(rep)) == -1) {
            perror("vigil-watch: MsgSend");
            return printed;
        }
        if (rep.lost > 0) {
            printf("(missed %u events)\n", rep.lost);
        }
        for (uint32_t i = 0; i < rep.n; i++) {
            const vg_event_t *e = &rep.ev[i];
            printf("%s%-7s%s bed %u  %-7s %s -> %s", colour((vg_level_t)e->to),
                   e->to == VG_LVL_NONE ? "CLEARED" : vg_level_name((vg_level_t)e->to), reset(), e->bed,
                   vg_src_name(e->src), vg_level_name((vg_level_t)e->from), vg_level_name((vg_level_t)e->to));
            if (e->value_ok) {
                printf("  value %.*f", e->src == VG_TEMP ? 1 : 0, e->value);
            }
            printf("\n");
            printed++;
        }
        fflush(stdout);
        *since = rep.next_seq;
        if (rep.n < VG_EVENTS_MAX) {
            return printed;
        }
    }
}

int main(int argc, char **argv)
{
    unsigned max_events = 0, timeout_s = 0, shown = 0;
    int show_history = 0, opt;
    while ((opt = getopt(argc, argv, "n:t:ah")) != -1) {
        switch (opt) {
        case 'n': max_events = (unsigned)atoi(optarg); break;
        case 't': timeout_s = (unsigned)atoi(optarg); break;
        case 'a': show_history = 1; break;
        default:
            fprintf(stderr, "usage: vigil-watch [-n events] [-t seconds] [-a]\n");
            return opt == 'h' ? 0 : 2;
        }
    }

    int srv = name_open(VG_SERVICE_NAME, 0);
    if (srv == -1) {
        fprintf(stderr, "vigil-watch: cannot find \"%s\" (%s). Is vigild running?\n",
                VG_SERVICE_NAME, strerror(errno));
        return 1;
    }
    int chid = ChannelCreate(0);
    int self = chid == -1 ? -1 : ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (self == -1) {
        perror("vigil-watch: cannot create a channel");
        return 1;
    }

    /* The secure event: a pulse on *our* channel that only the daemon may send. */
    vg_msg_subscribe_t sub;
    memset(&sub, 0, sizeof(sub));
    sub.h.type = VG_MSG_SUBSCRIBE;
    SIGEV_PULSE_INIT(&sub.ev, self, SIGEV_PULSE_PRIO_INHERIT, VG_WATCH_PULSE_CODE, 0);
    if (MsgRegisterEvent(&sub.ev, srv) == -1) {
        perror("vigil-watch: MsgRegisterEvent");
        return 1;
    }
    if (MsgSend(srv, &sub, sizeof(sub), NULL, 0) == -1) {
        perror("vigil-watch: subscribe");
        return 1;
    }

    uint64_t since = show_history ? 0 : VG_EVENTS_FROM_NOW;
    if (show_history) {
        shown += drain(srv, &since);
    } else {
        /* Learn the newest sequence number without printing the history. */
        vg_events_reply_t rep;
        vg_msg_get_events_t m;
        memset(&m, 0, sizeof(m));
        m.h.type = VG_MSG_GET_EVENTS;
        m.since_seq = VG_EVENTS_FROM_NOW;
        if (MsgSend(srv, &m, sizeof(m), &rep, sizeof(rep)) == -1) {
            perror("vigil-watch: MsgSend");
            return 1;
        }
        since = rep.next_seq;
    }
    fprintf(stderr, "vigil-watch: subscribed, waiting for alarm changes...\n");

    while (max_events == 0 || shown < max_events) {
        if (timeout_s > 0) {
            uint64_t ns = (uint64_t)timeout_s * 1000000000ull;
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &ns, NULL);
        }
        struct _pulse pulse;
        rcvid_t rcvid = MsgReceive(chid, &pulse, sizeof(pulse), NULL);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ETIMEDOUT) {
                fprintf(stderr, "vigil-watch: no alarm change for %u s\n", timeout_s);
                break;
            }
            perror("vigil-watch: MsgReceive");
            break;
        }
        if (rcvid == 0 && pulse.code == VG_WATCH_PULSE_CODE) {
            shown += drain(srv, &since);
        }
    }

    vg_msg_hdr_t bye = { VG_MSG_UNSUBSCRIBE, 0 };
    MsgSend(srv, &bye, sizeof(bye), NULL, 0);
    return max_events != 0 && shown < max_events ? 3 : 0;
}
