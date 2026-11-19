/*
 * vigild ingest thread: UDP (unicast or multicast) -> NMEA/AIS decode -> MsgSend().
 *
 * Decoding happens here, at the *lower* priority, so a flood of garbage or of
 * distant traffic costs the engine nothing but one tiny synchronous message
 * per decoded report. The engine (priority 40) preempts this thread whenever
 * its timer pulse fires, which is what keeps the alarm cycle on schedule.
 *
 * SPDX-License-Identifier: MIT
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/socket.h>
#include <unistd.h>

#include "vigild.h"

static int send_to_engine(int coid, const void *msg, size_t len)
{
    static unsigned long failures;
    if (MsgSend(coid, msg, len, NULL, 0) == -1) {
        if (failures++ % 1000 == 0) { /* rate-limit: never let logging become the load */
            lkd_log(SLOG2_ERROR, "MsgSend to engine failed: %s (%lu so far)", strerror(errno),
                    failures);
        }
        return -1;
    }
    return 0;
}

static void handle_line(int coid, vg_ais_reasm_t *reasm, const char *line, size_t len)
{
    atomic_fetch_add(&g_cnt.sentences, 1);

    vg_nmea_t s;
    if (vg_nmea_parse(line, len, &s) != VG_NMEA_OK) {
        atomic_fetch_add(&g_cnt.bad_sentences, 1);
        return;
    }

    vg_msg_fix_t fm;
    if (vg_nmea_ownship(&s, &fm.fix)) {
        fm.h.type = VG_MSG_FIX;
        fm.h.reserved = 0;
        send_to_engine(coid, &fm, sizeof(fm));
        return;
    }

    vg_msg_ais_t am;
    memset(&am, 0, sizeof(am));
    am.h.type = VG_MSG_AIS;
    int rc = vg_ais_feed(reasm, &s, lkd_now_ns() / 1000000u, &am.ais);
    if (rc == 1) {
        atomic_fetch_add(&g_cnt.ais_msgs, 1);
        send_to_engine(coid, &am, sizeof(am));
    } else if (rc < 0 && rc != VG_AIS_ERR_NOT_AIS && rc != VG_AIS_UNSUPPORTED) {
        atomic_fetch_add(&g_cnt.ais_errors, 1);
    }
}

static int open_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        lkd_log(SLOG2_CRITICAL, "ingest: socket(): %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int rcvbuf = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)g_opt.udp_port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        lkd_log(SLOG2_CRITICAL, "ingest: bind(udp %d): %s", g_opt.udp_port, strerror(errno));
        close(fd);
        return -1;
    }
    if (g_opt.mcast_group[0] != '\0') {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        if (inet_pton(AF_INET, g_opt.mcast_group, &mreq.imr_multiaddr) != 1) {
            lkd_log(SLOG2_ERROR, "ingest: bad multicast group '%s'", g_opt.mcast_group);
        } else {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == -1) {
                lkd_log(SLOG2_ERROR, "ingest: joining %s failed: %s", g_opt.mcast_group,
                        strerror(errno));
            } else {
                lkd_log(SLOG2_INFO, "ingest: joined multicast group %s", g_opt.mcast_group);
            }
        }
    }
    return fd;
}

void *lkd_ingest_thread(void *arg)
{
    (void)arg;
    static vg_ais_reasm_t reasm;
    vg_ais_reasm_init(&reasm);

    /* A side-channel connection to the engine's channel in our own process. */
    int coid = ConnectAttach(0, 0, g_engine_chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        lkd_log(SLOG2_CRITICAL, "ingest: ConnectAttach to engine failed: %s", strerror(errno));
        return NULL;
    }
    int fd = open_socket();
    if (fd == -1) {
        return NULL;
    }
    lkd_log(SLOG2_INFO, "ingest: listening on udp %d", g_opt.udp_port);

    char buf[2048];
    for (;;) {
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            lkd_log(SLOG2_ERROR, "ingest: recvfrom: %s", strerror(errno));
            usleep(100000);
            continue;
        }
        buf[n] = '\0';
        /* One datagram may carry several CR/LF separated sentences. */
        char *p = buf;
        while (p < buf + n) {
            char *eol = p;
            while (eol < buf + n && *eol != '\n' && *eol != '\r') {
                eol++;
            }
            if (eol > p) {
                handle_line(coid, &reasm, p, (size_t)(eol - p));
            }
            p = eol + 1;
        }
    }
    return NULL;
}
