/*
 * Vigil - native QNX message-passing protocol.
 *
 * The engine thread of vigild owns a named channel ("vigil", registered
 * with name_attach()). Everything that changes or queries the picture goes
 * through it as a synchronous message:
 *
 *   ingest thread  --MsgSend--> engine   decoded AIS reports and own-ship fixes
 *   vigil-ctl    --MsgSend--> engine   stats / alarms / tracks / config
 *   vigil-watch  --MsgSend--> engine   SUBSCRIBE with a *secure event*
 *   engine         --MsgDeliverEvent--> subscriber   a pulse when an alarm changes
 *
 * Rules of the road (QNX System Architecture guide, IPC chapter):
 *  - every message starts with a uint16_t type above _IO_MAX (the system range);
 *  - the engine never blocks on a client: it only replies, or delivers an event;
 *  - subscribers register their event with MsgRegisterEvent() against the
 *    engine's connection, so only the engine can fire it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VG_IPC_H
#define VG_IPC_H

#include <signal.h>
#include <stdint.h>
#include <sys/iomsg.h>

#include "../core/ais.h"
#include "../core/engine.h"
#include "../core/nmea.h"

#define VG_SERVICE_NAME "vigil"

#define VG_MSG_AIS         (_IO_MAX + 0x10) /* ingest -> engine: a decoded AIS report */
#define VG_MSG_FIX         (_IO_MAX + 0x11) /* ingest -> engine: an own-ship GNSS fix */
#define VG_MSG_SUBSCRIBE   (_IO_MAX + 0x20) /* client: deliver this event on alarm changes */
#define VG_MSG_UNSUBSCRIBE (_IO_MAX + 0x21)
#define VG_MSG_GET_EVENTS  (_IO_MAX + 0x22) /* client: alarm transitions since a sequence number */
#define VG_MSG_GET_TEXT    (_IO_MAX + 0x23) /* client: rendered stats / alarms / tracks / config */
#define VG_MSG_CONFIG      (_IO_MAX + 0x24) /* client: change one setting */

enum vg_text_kind {
    VG_TEXT_STATS  = 1,
    VG_TEXT_ALARMS = 2,
    VG_TEXT_TRACKS = 3,
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
    vg_ais_msg_t ais;
} vg_msg_ais_t;

typedef struct {
    vg_msg_hdr_t  h;
    vg_nmea_fix_t fix;
} vg_msg_fix_t;

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
    char         line[VG_CFG_LINE]; /* "warn_cpa_nm 1.5", or "reset_stats 1" */
} vg_msg_config_t;

/* What the engine thread can receive on its channel. */
typedef union {
    uint16_t            type;
    vg_msg_hdr_t        h;
    vg_msg_ais_t        ais;
    vg_msg_fix_t        fix;
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
