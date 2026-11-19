/*
 * clock-watch: subscribe to clock-rm with a secure event and print the value on every wake-up (docs/CODELAB.md).
 *
 *   clock-watch [-n count]      exit after count updates (default: run until Ctrl-C)
 *
 * Build: qcc -Vgcc_ntox86_64 -Wall -o clock-watch clock-watch.c     SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "clock_ipc.h"

int main(int argc, char **argv)
{
    int limit = argc == 3 && strcmp(argv[1], "-n") == 0 ? atoi(argv[2]) : 0;
    int server = name_open(CLOCK_NAME, 0);
    if (server == -1) {
        perror("name_open (is clock-rm running?)");
        return 1;
    }
    int chid = ChannelCreate(0);
    int self = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

    clock_subscribe_t sub;
    memset(&sub, 0, sizeof sub);
    sub.type = CLOCK_MSG_SUBSCRIBE;
    SIGEV_PULSE_INIT(&sub.ev, self, SIGEV_PULSE_PRIO_INHERIT, CLOCK_PULSE_CODE, 0);
    if (MsgRegisterEvent(&sub.ev, server) == -1) { /* only the server behind this connection may fire it */
        perror("MsgRegisterEvent");
        return 1;
    }
    if (MsgSend(server, &sub, sizeof sub, NULL, 0) == -1) {
        perror("subscribe");
        return 1;
    }
    fprintf(stderr, "clock-watch: subscribed, sleeping until the server fires the event\n");

    for (int got = 0; !limit || got < limit; got++) {
        struct _pulse p;
        if (MsgReceive(chid, &p, sizeof p, NULL) != 0) continue; /* only pulses arrive on this channel */
        char buf[64];
        int fd = open("/dev/clock/now", O_RDONLY);
        ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
        if (fd >= 0) close(fd);
        if (n < 0) n = 0;
        buf[n] = 0;
        printf("woken by pulse %d: %s", p.code, buf);
        fflush(stdout);
    }
    return 0;
}
