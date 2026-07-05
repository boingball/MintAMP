#ifndef RADIO_WORKER_IPC_H
#define RADIO_WORKER_IPC_H

#include <exec/types.h>
#include <exec/ports.h>

#define RADIO_WORKER_IPC_MAGIC 0x52574950UL /* RWIP */

#define RADIO_WORKER_EVENT_STARTED 1
#define RADIO_WORKER_EVENT_STATUS  2
#define RADIO_WORKER_EVENT_ERROR   3
#define RADIO_WORKER_EVENT_DONE    4
#define RADIO_WORKER_EVENT_STOP    5

typedef struct RadioWorkerIpcMessage {
    struct Message msg;
    ULONG magic;
    ULONG runId;
    ULONG event;
    LONG phase;
    LONG radioStatus;
    LONG radioActive;
    LONG radioBitrateKbps;
    LONG radioBufferedBytes;
    LONG radioMetaInt;
    char radioTitle[128];
    char radioStationName[128];
    char radioGenre[64];
    char radioStreamUrl[128];
    char radioContentType[64];
    char radioError[128];
} RadioWorkerIpcMessage;

#endif
