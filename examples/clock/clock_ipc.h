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
