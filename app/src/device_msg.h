#ifndef SC_DEVICEMSG_H
#define SC_DEVICEMSG_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DEVICE_MSG_MAX_SIZE (1 << 18) // 256k
// type: 1 byte; length: 4 bytes
#define DEVICE_MSG_TEXT_MAX_LENGTH (DEVICE_MSG_MAX_SIZE - 5)
#define SC_MIC_STATUS_TEXT_MAX_LENGTH 512
#define SC_MIC_PCM_MAX_SIZE 1920 // 20 ms, mono PCM16LE at 48 kHz

enum sc_mic_status {
    SC_MIC_STATUS_STOPPED,
    SC_MIC_STATUS_READY,
    SC_MIC_STATUS_ERROR,
};

enum sc_device_msg_type {
    DEVICE_MSG_TYPE_CLIPBOARD,
    DEVICE_MSG_TYPE_ACK_CLIPBOARD,
    DEVICE_MSG_TYPE_UHID_OUTPUT,
    DEVICE_MSG_TYPE_MIC_STATUS,
    DEVICE_MSG_TYPE_MIC_PCM,
    DEVICE_MSG_TYPE_CLOCK,
};

struct sc_device_msg {
    enum sc_device_msg_type type;
    union {
        struct {
            char *text; // owned, to be freed by free()
        } clipboard;
        struct {
            uint64_t sequence;
        } ack_clipboard;
        struct {
            uint16_t id;
            uint16_t size;
            uint8_t *data; // owned, to be freed by free()
        } uhid_output;
        struct {
            uint32_t session;
            uint8_t status;
            char text[SC_MIC_STATUS_TEXT_MAX_LENGTH + 1];
        } mic_status;
        struct {
            uint32_t session;
            int64_t pts; // Android monotonic microseconds of the first sample
            uint16_t size;
            uint8_t data[SC_MIC_PCM_MAX_SIZE];
        } mic_pcm;
        struct {
            uint32_t session;
            uint32_t sequence;
            int64_t device_us;
        } clock;
    };
};

// return the number of bytes consumed (0 for no msg available, -1 on error)
ssize_t
sc_device_msg_deserialize(const uint8_t *buf, size_t len,
                          struct sc_device_msg *msg);

void
sc_device_msg_destroy(struct sc_device_msg *msg);

#endif
