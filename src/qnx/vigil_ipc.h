/*
 * Vigil - native QNX message-passing protocol.
 *
 * The engine thread of vigild owns a named channel ("vigil", registered with name_attach()).
 * Everything that changes or queries the ward goes through it as a synchronous message:
 *
 *   ingest thread  --MsgSend--> engine   one decoded vital-sign frame
 *   vigil-ctl      --MsgSend--> engine   beds / alarms / stats / config, audio pause
 *   vigil-watch    --MsgSend--> engine   SUBSCRIBE with a *secure event*
 *   engine         --MsgDeliverEvent--> subscriber   a pulse when an alarm changes
 *
 * Rules of the road (QNX System Architecture guide, IPC chapter): every message starts with a
 * uint16_t type above _IO_MAX; the engine never blocks on a client, it only replies or delivers
 * an event; subscribers register their event with MsgRegisterEvent() against the engine's
 * connection, so only the engine can fire it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VG_IPC_H
#define VG_IPC_H

#include <signal.h>
#include <stdint.h>
#include <sys/iomsg.h>

#include "../core/alarms.h"
#include "../core/nmea.h"
#include "../core/vitals.h"

#define VG_SERVICE_NAME "vigil"

#define VG_MSG_SAMPLE      (_IO_MAX + 0x10) /* ingest -> engine: one decoded vital-sign frame */
#define VG_MSG_SUBSCRIBE   (_IO_MAX + 0x20) /* client: deliver this event on alarm changes */
#define VG_MSG_UNSUBSCRIBE (_IO_MAX + 0x21)
#define VG_MSG_GET_EVENTS  (_IO_MAX + 0x22) /* client: alarm transitions since a sequence number */
#define VG_MSG_GET_TEXT    (_IO_MAX + 0x23) /* client: rendered beds / alarms / stats / config */
#define VG_MSG_CONFIG      (_IO_MAX + 0x24) /* client: change one setting, or "pause 1" / "resume 1" / "reset_stats 1" */

enum vg_text_kind {
    VG_TEXT_STATS  = 1,
    VG_TEXT_ALARMS = 2,
    VG_TEXT_BEDS   = 3,
    VG_TEXT_CONFIG = 4
};

#define VG_TEXT_MAX    65536
#define VG_CFG_LINE    96
#define VG_EVENTS_MAX  32
#define VG_MAX_SUBSCRIBERS 8

typedef struct {
    uint16_t type;
    uint16_t reserved;
} vg_msg_hdr_t;

typedef struct {
    vg_msg_hdr_t h;
    vg_sample_t  sample;
} vg_msg_sample_t;

typedef struct {
    vg_msg_hdr_t    h;
    struct sigevent ev; /* already passed through MsgRegisterEvent() */
} vg_msg_subscribe_t;

typedef struct {
    vg_msg_hdr_t h;
    uint64_t     since_seq;
} vg_msg_get_events_t;

typedef struct {
    vg_msg_hdr_t h;
    uint32_t     kind; /* enum vg_text_kind */
} vg_msg_get_text_t;

typedef struct {
    vg_msg_hdr_t h;
    char         line[VG_CFG_LINE];
} vg_msg_config_t;

/* What the engine thread can receive on its channel. */
typedef union {
    uint16_t            type;
    vg_msg_hdr_t        h;
    vg_msg_sample_t     sample;
    vg_msg_subscribe_t  sub;
    vg_msg_get_events_t get_events;
    vg_msg_get_text_t   get_text;
    vg_msg_config_t     config;
} vg_msg_any_t;

/* Reply to VG_MSG_GET_EVENTS. MsgReply() sends only the used part of ev[]. */
typedef struct {
    uint32_t   n;
    uint32_t   lost;     /* events the reader missed because the ring overwrote them */
    uint64_t   next_seq; /* pass this as since_seq next time */
    vg_event_t ev[VG_EVENTS_MAX];
} vg_events_reply_t;

#endif /* VG_IPC_H */
