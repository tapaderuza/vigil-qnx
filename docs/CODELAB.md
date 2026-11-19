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

## 1. A resource manager in 40 lines

A resource manager is a process that registers a path. When a client calls `open`/`read`/`write` on it,
the kernel turns that into messages to you. The skeleton, from `src/qnx/resmgr.c`:

```c
#include <sys/iofunc.h>
#include <sys/dispatch.h>

static iofunc_attr_t attr;
static char text[64]; static size_t len;

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int st = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    if (ocb->offset == 0) len = (size_t)snprintf(text, sizeof text, "%ld\n", (long)time(NULL));
    size_t off = (size_t)ocb->offset, n = off < len ? len - off : 0;
    if (msg->i.nbytes >= 0 && n > (size_t)msg->i.nbytes) n = (size_t)msg->i.nbytes;
    _IO_SET_READ_NBYTES(ctp, n);
    if (n == 0) return _RESMGR_NPARTS(0);
    SETIOV(ctp->iov, text + off, n);
    ocb->offset += n;
    return _RESMGR_NPARTS(1);
}

int main(void)
{
    dispatch_t *dpp = dispatch_create();
    resmgr_attr_t rattr = { .nparts_max = 1, .msg_max_size = 2048 };
    resmgr_connect_funcs_t cf; resmgr_io_funcs_t iof;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iof.read = io_read;
    iofunc_attr_init(&attr, S_IFCHR | 0444, NULL, NULL);
    resmgr_attach(dpp, &rattr, "/dev/clock/now", _FTYPE_ANY, 0, &cf, &iof, &attr);
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) { ctp = dispatch_block(ctp); dispatch_handler(ctp); }
}
```

Build with `qcc -Vgcc_ntox86_64 -o clock-rm clock-rm.c`, copy it to the VM with `scp`, run it and
`cat /dev/clock/now`. Notes worth understanding:

* The library keeps the read position in `ocb->offset`, so `cat` (which reads until it gets 0 bytes) works.
* Re-rendering only at offset 0 gives each `open` a consistent snapshot. Vigil does this for a table
  that changes ten times a second (`render()` in `resmgr.c`).
* Add a write handler the same way (`iofunc_write_verify`, `resmgr_msgget` to copy the data out) to
  make a setting writable. Vigil forwards the write to its engine as a message, so the engine stays
  the only owner of its state (`io_write()` in `resmgr.c`).

## 2. Message passing between your own threads and processes

Give the daemon a named channel so other processes can find it, exactly as Vigil's engine does:

```c
name_attach_t *attach = name_attach(NULL, "clock", 0);      /* server */
...
rcvid_t rcvid = MsgReceive(attach->chid, &msg, sizeof msg, &info);
if (rcvid == 0) { /* a pulse: timer tick, or _PULSE_CODE_DISCONNECT */ }
else            { /* a message; type must be above _IO_MAX */ MsgReply(rcvid, EOK, NULL, 0); }

int coid = name_open("clock", 0);                           /* client */
MsgSend(coid, &req, sizeof req, &reply, sizeof reply);
```

Two rules that avoid deadlock: a client sends and the server replies, never the other way round; and
if the server must call a client later, it sends an *event*, not a message (next section).

## 3. Push updates with a secure event (no polling)

The client creates its own channel, builds a pulse event aimed at it, **registers** it against the
server's connection (so only that server can fire it), and sends it to the server:

```c
int chid = ChannelCreate(0);
int self = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
struct sigevent ev;
SIGEV_PULSE_INIT(&ev, self, SIGEV_PULSE_PRIO_INHERIT, MY_PULSE_CODE, 0);
MsgRegisterEvent(&ev, server_coid);
MsgSend(server_coid, &subscribe_msg_containing_ev, sizeof subscribe_msg, NULL, 0);
for (;;) { struct _pulse p; if (MsgReceive(chid, &p, sizeof p, NULL) == 0) { /* woken by the server */ } }
```

The server saves the `rcvid` of the subscribe message and later calls
`MsgDeliverEvent(saved_rcvid, &saved_ev)` when its value changes. The client sleeps in `MsgReceive` at
zero CPU until then. See `handle_subscribe()` in `src/qnx/vigild.c` and `tools/vigil_watch.c`.

## 4. Drive it with a timer pulse

For periodic work, create a timer whose expiry is a pulse on your own channel, so the same
`MsgReceive` loop handles messages and ticks:

```c
struct sigevent ev; SIGEV_PULSE_INIT(&ev, self_coid, prio, TICK, 0);
timer_t t; timer_create(CLOCK_MONOTONIC, &ev, &t);
struct itimerspec its = { .it_value = {0, 100000000}, .it_interval = {0, 100000000} };
timer_settime(t, 0, &its, NULL);          /* a tick every 100 ms */
```

Give that thread a high `SCHED_FIFO` priority and the work keeps its schedule while lower-priority
threads (network, web) take whatever CPU is left.

## 5. Exercises

1. Add `/dev/clock/format` that you can write `epoch` or `iso` to.
2. Notify subscribers only when the value changes by more than a threshold.
3. Log each change with `slog2f()` and read it with `slog2info`.
4. Run the real thing: clone this repository, `make qnx`, and read `src/qnx/vigild.c`.

## Troubleshooting

* `name_attach` fails with `EPERM`: run the daemon as root (it needs the public-channel ability).
* `cat` shows nothing: check `ls /dev/clock`; the daemon must still be running.
* `SCHED_FIFO` priority refused: Vigil logs a warning and continues at the inherited priority.

## Bonus: run your host tests on the target

Build the same unit-test binary with `qcc` and run it on the QNX VM (`make qnx`, `scp`, run). It is the cheapest
way to find platform differences. In this project it exposed one: `strtod("36.8")` on QNX 8.0 returns a value one
ulp above the compiler literal, so `strtod(s) == 36.8` is false there and true on a PC. The fix is to normalise
decimals you compare against limits (`round(v * 10.0) / 10.0`), and to keep the test that caught it.
