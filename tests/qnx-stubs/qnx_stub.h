/*
 * Stand-in declarations for the QNX 8.0 headers used by src/qnx and tools/.
 *
 * PURPOSE: let `make check-qnx-syntax` type-check the QNX-specific sources with
 * a plain host compiler (gcc -fsyntax-only) on machines that have no QNX SDP.
 * The signatures are transcribed from the QNX 8.0 Library Reference
 * (MsgReceive, MsgReply, MsgSend, MsgError, MsgSendPulse, MsgDeliverEvent,
 * MsgRegisterEvent, ChannelCreate, ConnectAttach, TimerTimeout, name_attach,
 * name_open, resmgr_attach, dispatch_*, iofunc_*, slog2_*).
 *
 * THIS IS NOT A SUBSTITUTE FOR BUILDING WITH qcc. It never links and never
 * runs; it only catches typos, missing includes and type mismatches against
 * the documented prototypes. The real proof is `make qnx` and the QEMU run.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VG_QNX_STUB_H
#define VG_QNX_STUB_H

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>

#ifndef EOK
#define EOK 0
#endif

/* ---- <sys/neutrino.h> / <sys/iomsg.h> ---------------------------------- */
typedef long rcvid_t;

struct _msg_info {
    uint32_t nd, srcnd;
    pid_t    pid;
    int32_t  tid, chid, scoid, coid, priority;
    int16_t  flags;
    int32_t  msglen;
    int32_t  srcmsglen;
    int32_t  dstmsglen;
};

struct _pulse {
    uint16_t     type;
    uint16_t     subtype;
    int8_t       code;
    uint8_t      zero[3];
    union sigval value;
    int32_t      scoid;
};

#define _IO_CONNECT 0x100
#define _IO_MAX 0x1ff
#define _PULSE_CODE_MINAVAIL 0
#define _PULSE_CODE_DISCONNECT (-33)
#define _PULSE_CODE_UNBLOCK (-32)
#define _NTO_SIDE_CHANNEL 0x40000000u
#define _NTO_TIMEOUT_RECEIVE (1u << 3)
#define SIGEV_PULSE_PRIO_INHERIT (-1)
#define SIGEV_PULSE_INIT(ev, coid, prio, code, val)                                          \
    do {                                                                                     \
        memset((ev), 0, sizeof(*(ev)));                                                      \
        (void)(coid); (void)(prio); (void)(code); (void)(val);                               \
    } while (0)

rcvid_t MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info);
int     MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes);
int     MsgError(rcvid_t rcvid, int error);
long    MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes);
int     MsgSendPulse(int coid, int priority, int code, long value);
int     MsgDeliverEvent(rcvid_t rcvid, const struct sigevent *event);
int     MsgRegisterEvent(struct sigevent *event, int coid);
int     ChannelCreate(unsigned flags);
int     ConnectAttach(uint32_t reserved, pid_t pid, int chid, unsigned index, int flags);
int     ConnectDetach(int coid);
int     TimerTimeout(clockid_t id, uint32_t flags, const struct sigevent *notify,
                     const uint64_t *ntime, uint64_t *otime);

/* ---- <sys/dispatch.h>: name_attach -------------------------------------- */
typedef struct _dispatch dispatch_t;
typedef struct _dispatch_context dispatch_context_t;
typedef struct _name_attach {
    dispatch_t *dpp;
    int         chid;
    int         mntid;
    int         zero[2];
} name_attach_t;

name_attach_t *name_attach(dispatch_t *dpp, const char *path, unsigned flags);
int            name_detach(name_attach_t *attach, unsigned flags);
int            name_open(const char *name, int flags);
int            name_close(int coid);

/* ---- <sys/iofunc.h> / <sys/resmgr.h> ------------------------------------ */
typedef struct iovec iov_t;
#define SETIOV(iov, base, len)                                                               \
    do { (iov)->iov_base = (void *)(base); (iov)->iov_len = (len); } while (0)

struct _client_info;

typedef struct {
    uint16_t type, combine_len;
    uint32_t nbytes; /* unsigned in the real <sys/iomsg.h> (found by the first qcc build) */
    uint32_t xtype;
} io_read_i_t;
typedef struct { io_read_i_t i; } io_read_t;
typedef struct {
    uint16_t type, combine_len;
    int32_t  nbytes;
    uint32_t xtype;
} io_write_i_t;
typedef struct { io_write_i_t i; } io_write_t;

typedef struct {
    uint32_t flags;
    mode_t   mode;
} iofunc_attr_t;

typedef struct {
    iofunc_attr_t *attr;
    off_t          offset;
} iofunc_ocb_t;
#define RESMGR_OCB_T iofunc_ocb_t

typedef struct {
    rcvid_t          rcvid;
    struct _msg_info info;
    long             status;
    iov_t            iov[8];
} resmgr_context_t;

typedef struct _resmgr_attr {
    unsigned flags;
    unsigned nparts_max;
    size_t   msg_max_size;
    int    (*other_func)(resmgr_context_t *, void *msg);
    unsigned reserved[4];
} resmgr_attr_t;

typedef struct {
    unsigned nfuncs;
    void    *placeholder[16];
} resmgr_connect_funcs_t;

typedef struct {
    unsigned nfuncs;
    int    (*read)(resmgr_context_t *, io_read_t *, iofunc_ocb_t *);
    int    (*write)(resmgr_context_t *, io_write_t *, iofunc_ocb_t *);
    void    *placeholder[40];
} resmgr_io_funcs_t;

enum _file_type { _FTYPE_ANY = 0 };

#define _RESMGR_CONNECT_NFUNCS 12
#define _RESMGR_IO_NFUNCS 36
#define _RESMGR_NPARTS(n) (-(n))
#define _IO_SET_READ_NBYTES(ctp, n) do { (ctp)->status = (long)(n); } while (0)
#define _IO_SET_WRITE_NBYTES(ctp, n) do { (ctp)->status = (long)(n); } while (0)
#define _IO_XTYPE_MASK 0xffu
#define _IO_XTYPE_NONE 0x0u
#define IOFUNC_ATTR_ATIME 0x1u
#define IOFUNC_ATTR_DIRTY_TIME 0x100u

void iofunc_func_init(unsigned nconnect, resmgr_connect_funcs_t *connect, unsigned nio,
                      resmgr_io_funcs_t *io);
void iofunc_attr_init_sized(iofunc_attr_t *attr, mode_t mode, iofunc_attr_t *dattr,
                            struct _client_info *info, size_t size);
#define iofunc_attr_init(attr, mode, dattr, info) \
    iofunc_attr_init_sized((attr), (mode), (dattr), (info), sizeof(iofunc_attr_t))
int     iofunc_read_verify(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb,
                           int *nonblock);
int     iofunc_write_verify(resmgr_context_t *ctp, io_write_t *msg, iofunc_ocb_t *ocb,
                            int *nonblock);
ssize_t resmgr_msgget(resmgr_context_t *const ctp, void *msg, size_t size, size_t offset);
int     resmgr_attach(dispatch_t *dpp, resmgr_attr_t *attr, const char *path,
                      enum _file_type file_type, unsigned flags,
                      const resmgr_connect_funcs_t *connect_funcs,
                      const resmgr_io_funcs_t *io_funcs, iofunc_attr_t *handle);

dispatch_t         *dispatch_create(void);
dispatch_context_t *dispatch_context_alloc(dispatch_t *dpp);
dispatch_context_t *dispatch_block(dispatch_context_t *ctp);
int                 dispatch_handler(dispatch_context_t *ctp);

/* ---- <sys/slog2.h> ------------------------------------------------------ */
typedef struct slog2_buffer_obj *slog2_buffer_t;
#define SLOG2_MAX_BUFFERS 4
#define SLOG2_SHUTDOWN 0
#define SLOG2_CRITICAL 1
#define SLOG2_ERROR 2
#define SLOG2_WARNING 3
#define SLOG2_NOTICE 4
#define SLOG2_INFO 5
#define SLOG2_DEBUG1 6
#define SLOG2_DEBUG2 7

typedef struct {
    const char *buffer_name;
    int         num_pages;
} slog2_buffer_config_t;

typedef struct {
    int                   num_buffers;
    const char           *buffer_set_name;
    uint8_t               verbosity_level;
    slog2_buffer_config_t buffer_config[SLOG2_MAX_BUFFERS];
    uint32_t              max_retries;
} slog2_buffer_set_config_t;

int slog2_register(slog2_buffer_set_config_t *config, slog2_buffer_t *handles, uint32_t flags);
int slog2f(slog2_buffer_t buffer, uint16_t code, uint8_t severity, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

#endif /* VG_QNX_STUB_H */
