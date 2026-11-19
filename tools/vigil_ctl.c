/*
 * vigil-ctl - talk to vigild with native QNX message passing.
 *
 *   vigil-ctl stats            counters, cycle time, timer-wake latency
 *   vigil-ctl alarms           active alarms with COLREGs advice
 *   vigil-ctl tracks           every vessel, most urgent first
 *   vigil-ctl config           thresholds
 *   vigil-ctl events [SEQ]     alarm transitions after sequence number SEQ
 *   vigil-ctl set "key value"  change a threshold, e.g. "warn_cpa_nm 2"
 *   vigil-ctl reset            reset the max-latency / max-cycle statistics
 *
 * It finds the daemon by name (name_open) and sends it synchronous messages;
 * no sockets, no files. Compare with `cat /dev/vigil/alarms`, which reaches
 * the same data through the resource manager instead.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "../src/qnx/vigil_ipc.h"

static char g_reply[VG_TEXT_MAX];

static int get_text(int coid, uint32_t kind)
{
    vg_msg_get_text_t m;
    memset(&m, 0, sizeof(m));
    m.h.type = VG_MSG_GET_TEXT;
    m.kind = kind;
    long n = MsgSend(coid, &m, sizeof(m), g_reply, sizeof(g_reply) - 1);
    if (n == -1) {
        perror("vigil-ctl: MsgSend");
        return 1;
    }
    g_reply[n] = '\0';
    fputs(g_reply, stdout);
    return 0;
}

static int get_events(int coid, uint64_t since)
{
    static vg_events_reply_t rep;
    vg_msg_get_events_t m;
    memset(&m, 0, sizeof(m));
    m.h.type = VG_MSG_GET_EVENTS;
    m.since_seq = since;
    if (MsgSend(coid, &m, sizeof(m), &rep, sizeof(rep)) == -1) {
        perror("vigil-ctl: MsgSend");
        return 1;
    }
    if (rep.lost > 0) {
        printf("(missed %u older events)\n", rep.lost);
    }
    vg_render_events(rep.ev, rep.n, g_reply, sizeof(g_reply));
    fputs(g_reply, stdout);
    printf("next_seq %llu\n", (unsigned long long)rep.next_seq);
    return 0;
}

static int set_config(int coid, const char *line)
{
    vg_msg_config_t m;
    memset(&m, 0, sizeof(m));
    m.h.type = VG_MSG_CONFIG;
    snprintf(m.line, sizeof(m.line), "%s", line);
    char reply[128];
    memset(reply, 0, sizeof(reply));
    long st = MsgSend(coid, &m, sizeof(m), reply, sizeof(reply) - 1);
    if (st == -1) {
        perror("vigil-ctl: MsgSend");
        return 1;
    }
    fputs(reply, st == 0 ? stdout : stderr);
    return st == 0 ? 0 : 1;
}

static int usage(void)
{
    fprintf(stderr, "usage: vigil-ctl stats|alarms|tracks|config|reset\n"
                    "       vigil-ctl events [SEQ]\n"
                    "       vigil-ctl set \"warn_cpa_nm 2\"\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return usage();
    }
    int coid = name_open(VG_SERVICE_NAME, 0);
    if (coid == -1) {
        fprintf(stderr, "vigil-ctl: cannot find \"%s\" (%s). Is vigild running?\n",
                VG_SERVICE_NAME, strerror(errno));
        return 1;
    }
    int rc;
    if (strcmp(argv[1], "stats") == 0) {
        rc = get_text(coid, VG_TEXT_STATS);
    } else if (strcmp(argv[1], "alarms") == 0) {
        rc = get_text(coid, VG_TEXT_ALARMS);
    } else if (strcmp(argv[1], "tracks") == 0) {
        rc = get_text(coid, VG_TEXT_TRACKS);
    } else if (strcmp(argv[1], "config") == 0) {
        rc = get_text(coid, VG_TEXT_CONFIG);
    } else if (strcmp(argv[1], "events") == 0) {
        rc = get_events(coid, argc > 2 ? strtoull(argv[2], NULL, 10) : 0);
    } else if (strcmp(argv[1], "set") == 0 && argc == 3) {
        rc = set_config(coid, argv[2]);
    } else if (strcmp(argv[1], "reset") == 0) {
        rc = set_config(coid, "reset_stats 1");
    } else {
        rc = usage();
    }
    name_close(coid);
    return rc;
}
