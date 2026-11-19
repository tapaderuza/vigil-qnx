/*
 * clock-rm: a tiny resource manager with push updates (docs/CODELAB.md).
 *
 *   /dev/clock/now     read: "tick N"; N goes up once a second
 *   (name "clock")     a client sends CLOCK_MSG_SUBSCRIBE with a registered event and is woken on every tick
 *
 * Build: qcc -Vgcc_ntox86_64 -Wall -o clock-rm clock-rm.c     SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "clock_ipc.h"

#define TICK_PULSE (_PULSE_CODE_MINAVAIL + 2)

static volatile unsigned g_tick;
static iofunc_attr_t g_attr;

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int st = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    char text[32];
    size_t len = (size_t)snprintf(text, sizeof text, "tick %u\n", g_tick);
    size_t off = (size_t)ocb->offset, n = off < len ? len - off : 0;
    if (n > (size_t)msg->i.nbytes) n = (size_t)msg->i.nbytes;
    _IO_SET_READ_NBYTES(ctp, n);
    if (n == 0) return _RESMGR_NPARTS(0);
    SETIOV(ctp->iov, text + off, n);
    ocb->offset += (off_t)n;
    return _RESMGR_NPARTS(1);
}

static void *rm_thread(void *arg)
{
    (void)arg;
    dispatch_t *dpp = dispatch_create();
    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    resmgr_connect_funcs_t cf;
    resmgr_io_funcs_t iof;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iof.read = io_read;
    iofunc_attr_init(&g_attr, S_IFCHR | 0444, NULL, NULL);
    if (resmgr_attach(dpp, &rattr, "/dev/clock/now", _FTYPE_ANY, 0, &cf, &iof, &g_attr) == -1) {
        perror("resmgr_attach");
        exit(1);
    }
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        ctp = dispatch_block(ctp);
        dispatch_handler(ctp);
    }
    return NULL;
}

int main(void)
{
    name_attach_t *at = name_attach(NULL, CLOCK_NAME, 0);
    if (!at) {
        perror("name_attach (run as root)");
        return 1;
    }
    pthread_t th;
    pthread_create(&th, NULL, rm_thread, NULL);

    /* a timer whose expiry is a pulse on our own channel: one MsgReceive loop handles ticks and messages */
    int self = ConnectAttach(0, 0, at->chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent tev;
    SIGEV_PULSE_INIT(&tev, self, SIGEV_PULSE_PRIO_INHERIT, TICK_PULSE, 0);
    timer_t timer;
    timer_create(CLOCK_MONOTONIC, &tev, &timer);
    struct itimerspec its = { .it_value = { 1, 0 }, .it_interval = { 1, 0 } };
    timer_settime(timer, 0, &its, NULL);

    rcvid_t sub_rcvid = 0;
    struct sigevent sub_ev;
    int subscribed = 0;
    fprintf(stderr, "clock-rm: serving /dev/clock/now and name \"%s\"\n", CLOCK_NAME);

    for (;;) {
        union { uint16_t type; struct _pulse pulse; clock_subscribe_t sub; } m;
        rcvid_t rcvid = MsgReceive(at->chid, &m, sizeof m, NULL);
        if (rcvid == 0) {
            if (m.pulse.code == TICK_PULSE) {
                g_tick++;
                if (subscribed) MsgDeliverEvent(sub_rcvid, &sub_ev);
            }
        } else if (rcvid > 0) {
            if (m.type == CLOCK_MSG_SUBSCRIBE) {
                sub_rcvid = rcvid; /* stays valid for MsgDeliverEvent() after we reply */
                sub_ev = m.sub.ev;
                subscribed = 1;
                MsgReply(rcvid, EOK, NULL, 0);
            } else if (m.type == _IO_CONNECT) {
                MsgReply(rcvid, EOK, NULL, 0); /* name_open() may probe with a connect message */
            } else {
                MsgError(rcvid, ENOSYS);
            }
        }
    }
}
