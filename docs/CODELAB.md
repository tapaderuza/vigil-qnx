# Codelab: your first QNX 8.0 resource manager, with live updates by secure event

**Time:** about 60 minutes. **You need:** QNX SDP 8.0 (free, QNX Everywhere), a QNX 8.0 target
(QEMU is fine), and basic C. Everything here is taken from the working code in this repository.

**You will build** a tiny daemon, `clock-rm`, that (1) publishes a value as a file, `/dev/clock/now`,
that any tool can `cat`; (2) accepts a write to change a setting; and (3) lets a client subscribe with
a *secure event* so the kernel wakes it when the value changes, with no polling.

## 0. Set up a QNX VM on a Windows 11 PC

The official QEMU image targets Ubuntu, and on Windows 11 the easiest route is Ubuntu 24.04 in WSL2:

```powershell
wsl --install -d Ubuntu-24.04
```

Inside Ubuntu install QEMU (`sudo apt install qemu-system-x86 qemu-utils`), install the QNX Software
Center and QNX SDP 8.0 (you need a myQNX account and the free non-commercial licence), then:

```bash
source ~/qnx800/qnxsdp-env.sh
mkdir qnx-vm && cd qnx-vm
mkqnximage --type=qemu --arch=x86_64 --build
mkqnximage --run=-h            # boots headless
mkqnximage --getip             # prints the VM address
```

Check WSL2 exposes KVM (`ls /dev/kvm`); without it QEMU still works, just slowly.

## 1. Get the three source files

Everything below is in this repository under `examples/clock/` and was built with `qcc` and run on QNX 8.0.
From a clean clone:

```bash
git clone https://github.com/tapaderuza/vigil-qnx.git
cd vigil-qnx/examples/clock
```

The shared header (message type, name and pulse code):

```c
// clock_ipc.h
/* Shared by clock-rm and clock-watch (docs/CODELAB.md). SPDX-License-Identifier: MIT */
#ifndef CLOCK_IPC_H
#define CLOCK_IPC_H

#include <signal.h>
#include <sys/neutrino.h>

#define CLOCK_NAME "clock"                /* name_attach() / name_open() */
#define CLOCK_MSG_SUBSCRIBE (_IO_MAX + 1) /* message types must be above _IO_MAX */
#define CLOCK_PULSE_CODE (_PULSE_CODE_MINAVAIL + 1)

typedef struct {
    uint16_t type;
    struct sigevent ev; /* registered with MsgRegisterEvent() before it is sent */
} clock_subscribe_t;

#endif
```

## 2. The server: a resource manager, a named channel and a timer pulse

A resource manager is a process that registers a path. When a client calls `open`/`read` on it, the kernel turns that
into messages to you. This server also registers the name `clock`, so other processes can reach it by message, and
creates a timer whose expiry is a pulse on that same channel, so one `MsgReceive` loop handles ticks and messages.

```c
// clock-rm.c
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
```

Things worth understanding:

* The library keeps the read position in `ocb->offset`, so `cat` (which reads until it gets 0 bytes) works.
* A client sends and the server replies, never the other way round. When the server must call a client later it
  fires an *event*; that is why the subscribe message carries one (next section).
* The `rcvid` of the subscribe message stays valid for `MsgDeliverEvent()` after the reply, because the client
  registered the event with `MsgRegisterEvent()`.

## 3. The client: subscribe with a secure event, no polling

The client creates its own channel, builds a pulse event aimed at it, **registers** it against the server's
connection (so only that server can fire it) and sends it. Then it sleeps in `MsgReceive` at zero CPU until the
server fires the event.

```c
// clock-watch.c
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
```

## 4. Build and run

On the machine with the SDP (Ubuntu or WSL2), then copy to the VM (`mkqnximage --getip`, or the forwarded SSH
port of your QEMU):

```bash
source ~/qnx800/qnxsdp-env.sh
qcc -Vgcc_ntox86_64 -Wall -Wextra -o clock-rm clock-rm.c
qcc -Vgcc_ntox86_64 -Wall -Wextra -o clock-watch clock-watch.c
scp clock-rm clock-watch root@<vm-ip>:/tmp/        # add -P <port> when the SSH port is forwarded
```

On the QNX target (as root):

```sh
chmod +x /tmp/clock-rm /tmp/clock-watch
/tmp/clock-rm &                  # prints: clock-rm: serving /dev/clock/now and name "clock"
cat /dev/clock/now               # tick 2
/tmp/clock-watch -n 3            # three updates, one per second
slay clock-rm
```

Expected output (the tick numbers depend on how long the server has been running):

```
tick 2
clock-watch: subscribed, sleeping until the server fires the event
woken by pulse 1: tick 3
woken by pulse 1: tick 4
woken by pulse 1: tick 5
```

## 5. Exercises

1. Add a write handler (`iofunc_write_verify`, `resmgr_msgget`) so a write to `/dev/clock/now` resets the counter.
2. Notify subscribers only when the value changes by more than a threshold.
3. Log each change with `slog2f()` and read it with `slog2info`.
4. Run the real thing: `make qnx` and read `src/qnx/vigild.c`, which does this with a 10 Hz timer, many subscribers and an event ring.

## Troubleshooting

* `name_attach` fails with `EPERM`: run the daemon as root (it needs the public-channel ability).
* `cat` shows nothing: check `ls /dev/clock`; the server must still be running.
* `scp: dest open ... Failure` when copying again: the old process still runs; `slay clock-rm` first.

## Bonus: run your host tests on the target

Build the same unit-test binary with `qcc` and run it on the QNX VM (`make qnx`, `scp`, run). It is the cheapest
way to find platform differences. In this project it exposed one: `strtod("36.8")` on QNX 8.0 returns a value one
ulp above the compiler literal, so `strtod(s) == 36.8` is false there and true on a PC. The fix is to normalise
decimals you compare against limits (`round(v * 10.0) / 10.0`), and to keep the test that caught it.
