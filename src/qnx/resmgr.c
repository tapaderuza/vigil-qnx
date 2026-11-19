/*
 * vigild resource manager: the picture as files under /dev/vigil.
 *
 *   cat /dev/vigil/tracks    every vessel, most urgent first
 *   cat /dev/vigil/alarms    current WARNING / DANGER list with COLREGs advice
 *   cat /dev/vigil/stats     counters, cycle time and timer-wake latency
 *   cat /dev/vigil/events    recent alarm transitions
 *   cat /dev/vigil/config    alarm thresholds
 *   echo "warn_cpa_nm 2" > /dev/vigil/config      (also: reset_stats 1)
 *
 * Any tool that can read a file now speaks to the engine. Each open() reads a
 * snapshot taken when the client reads offset 0, so a `cat` of a table that
 * changes ten times a second still prints a consistent document.
 *
 * This thread only reads the published picture; the one write path (config)
 * is forwarded to the engine as a message, so the engine stays the sole
 * owner of its state.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/stat.h>

#include "vigild.h"

#define VG_RM_TEXT 65536

enum { K_TRACKS, K_ALARMS, K_STATS, K_EVENTS, K_CONFIG, K_COUNT };

static const struct {
    const char *name;
    mode_t      mode;
} k_dev[K_COUNT] = {
    { "tracks", 0444 }, { "alarms", 0444 }, { "stats", 0444 }, { "events", 0444 },
    { "config", 0644 },
};

typedef struct {
    iofunc_attr_t base; /* must be first: the library hands us this as the handle */
    int           kind;
} vg_attr_t;

static vg_attr_t  g_attr[K_COUNT];
static char       g_text[K_COUNT][VG_RM_TEXT];
static size_t     g_len[K_COUNT];
static lkd_pub_t  g_snap;
static vg_event_t g_events[VG_EVENT_RING];
static int        g_engine_coid = -1;

static void render(int kind)
{
    if (kind < 0 || kind >= K_COUNT) {
        return;
    }
    lkd_pub_snapshot(&g_snap);
    char *buf = g_text[kind];
    switch (kind) {
    case K_TRACKS:
        g_len[kind] = vg_render_tracks(&g_snap.pic, buf, VG_RM_TEXT);
        break;
    case K_ALARMS:
        g_len[kind] = vg_render_alarms(&g_snap.pic, buf, VG_RM_TEXT);
        break;
    case K_STATS:
        g_len[kind] = vg_render_stats(&g_snap.pic, &g_snap.cfg, buf, VG_RM_TEXT);
        break;
    case K_EVENTS: {
        uint32_t n = vg_events_since(&g_snap.events, 0, g_events, VG_EVENT_RING, NULL);
        g_len[kind] = vg_render_events(g_events, n, buf, VG_RM_TEXT);
        break;
    }
    case K_CONFIG:
        g_len[kind] = vg_render_config(&g_snap.cfg, buf, VG_RM_TEXT);
        break;
    }
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    const vg_attr_t *a = (const vg_attr_t *)ocb->attr;
    const int kind = a->kind;
    if (ocb->offset == 0) {
        render(kind); /* a fresh read of the file: take a new snapshot */
    }
    size_t total = g_len[kind];
    size_t off = (size_t)ocb->offset;
    size_t nbytes = 0;
    if (off < total) {
        nbytes = total - off;
        if (nbytes > (size_t)msg->i.nbytes) {
            nbytes = (size_t)msg->i.nbytes;
        }
    }
    _IO_SET_READ_NBYTES(ctp, nbytes);
    if (nbytes > 0) {
        SETIOV(ctp->iov, g_text[kind] + off, nbytes);
        ocb->offset += (off_t)nbytes;
        ocb->attr->flags |= IOFUNC_ATTR_ATIME | IOFUNC_ATTR_DIRTY_TIME;
        return _RESMGR_NPARTS(1);
    }
    return _RESMGR_NPARTS(0);
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    size_t n = msg->i.nbytes;
    if (n == 0) {
        _IO_SET_WRITE_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    if (n >= VG_CFG_LINE) {
        return EMSGSIZE;
    }
    vg_msg_config_t m;
    memset(&m, 0, sizeof(m));
    m.h.type = VG_MSG_CONFIG;
    if (resmgr_msgget(ctp, m.line, n, sizeof(msg->i)) != (ssize_t)n) {
        return EIO;
    }

    char reply[128];
    memset(reply, 0, sizeof(reply));
    long st = MsgSend(g_engine_coid, &m, sizeof(m), reply, sizeof(reply) - 1);
    if (st == -1) {
        return errno;
    }
    if (st != 0) {
        return EINVAL; /* the engine's explanation is in `slog2info -b vigil` */
    }
    _IO_SET_WRITE_NBYTES(ctp, n);
    return _RESMGR_NPARTS(0);
}

void *lkd_resmgr_thread(void *arg)
{
    (void)arg;
    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) {
        lkd_log(SLOG2_CRITICAL, "resmgr: dispatch_create: %s", strerror(errno));
        return NULL;
    }
    g_engine_coid = ConnectAttach(0, 0, g_engine_chid, _NTO_SIDE_CHANNEL, 0);
    if (g_engine_coid == -1) {
        lkd_log(SLOG2_CRITICAL, "resmgr: cannot connect to the engine: %s", strerror(errno));
        return NULL;
    }

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;

    resmgr_connect_funcs_t connect_funcs;
    resmgr_io_funcs_t      io_funcs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;
    io_funcs.write = io_write;

    for (int k = 0; k < K_COUNT; k++) {
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", g_opt.dev_prefix, k_dev[k].name);
        iofunc_attr_init(&g_attr[k].base, S_IFCHR | k_dev[k].mode, NULL, NULL);
        g_attr[k].kind = k;
        if (resmgr_attach(dpp, &rattr, path, _FTYPE_ANY, 0, &connect_funcs, &io_funcs,
                          &g_attr[k].base) == -1) {
            lkd_log(SLOG2_CRITICAL, "resmgr: cannot attach %s: %s", path, strerror(errno));
            return NULL;
        }
    }
    lkd_log(SLOG2_INFO, "resmgr: serving %s/{tracks,alarms,stats,events,config}",
            g_opt.dev_prefix);

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        lkd_log(SLOG2_CRITICAL, "resmgr: dispatch_context_alloc: %s", strerror(errno));
        return NULL;
    }
    for (;;) {
        ctp = dispatch_block(ctp);
        if (ctp == NULL) {
            lkd_log(SLOG2_ERROR, "resmgr: dispatch_block: %s", strerror(errno));
            return NULL;
        }
        dispatch_handler(ctp);
    }
}
