/*
 * vigild ingest thread: UDP (unicast or multicast) -> frame decode -> MsgSend().
 *
 * Decoding happens here, at the *lower* priority, so a flood of garbage frames costs the engine
 * nothing but one tiny synchronous message per valid frame. The engine (priority 40) preempts
 * this thread whenever its timer pulse fires, which keeps the alarm cycle on schedule.
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

static void handle_line(int coid, const char *line, size_t len)
{
    atomic_fetch_add(&g_cnt.samples, 1);
    vg_nmea_t s;
    vg_msg_sample_t m;
    memset(&m, 0, sizeof(m));
    if (vg_nmea_parse(line, len, &s) != VG_NMEA_OK || !vg_sample_parse(&s, &m.sample)) {
        atomic_fetch_add(&g_cnt.bad_frames, 1);
        return;
    }
    m.h.type = VG_MSG_SAMPLE;
    static unsigned long failures;
    if (MsgSend(coid, &m, sizeof(m), NULL, 0) == -1 && failures++ % 1000 == 0) {
        vgd_log(SLOG2_ERROR, "MsgSend to engine failed: %s (%lu so far)", strerror(errno), failures);
    }
}

static int open_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        vgd_log(SLOG2_CRITICAL, "ingest: socket(): %s", strerror(errno));
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
        vgd_log(SLOG2_CRITICAL, "ingest: bind(udp %d): %s", g_opt.udp_port, strerror(errno));
        close(fd);
        return -1;
    }
    if (g_opt.mcast_group[0] != '\0') {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        if (inet_pton(AF_INET, g_opt.mcast_group, &mreq.imr_multiaddr) != 1) {
            vgd_log(SLOG2_ERROR, "ingest: bad multicast group '%s'", g_opt.mcast_group);
        } else {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == -1) {
                vgd_log(SLOG2_ERROR, "ingest: joining %s failed: %s", g_opt.mcast_group, strerror(errno));
            }
        }
    }
    return fd;
}

void *vgd_ingest_thread(void *arg)
{
    (void)arg;
    int coid = ConnectAttach(0, 0, g_engine_chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        vgd_log(SLOG2_CRITICAL, "ingest: ConnectAttach to engine failed: %s", strerror(errno));
        return NULL;
    }
    int fd = open_socket();
    if (fd == -1) {
        return NULL;
    }
    vgd_log(SLOG2_INFO, "ingest: listening on udp %d", g_opt.udp_port);

    char buf[2048];
    for (;;) {
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            vgd_log(SLOG2_ERROR, "ingest: recvfrom: %s", strerror(errno));
            usleep(100000);
            continue;
        }
        buf[n] = '\0';
        /* One datagram may carry several CR/LF separated frames. */
        char *p = buf;
        while (p < buf + n) {
            char *eol = p;
            while (eol < buf + n && *eol != '\n' && *eol != '\r') {
                eol++;
            }
            if (eol > p) {
                handle_line(coid, p, (size_t)(eol - p));
            }
            p = eol + 1;
        }
    }
    return NULL;
}
