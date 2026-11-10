#include "device_msg.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "util/binary.h"
#include "util/log.h"

// Microphone status is a bounded UTF-8 diagnostic, not an arbitrary binary
// payload. Reject truncated/overlong sequences, surrogates and embedded NULs.
static bool
valid_status_utf8(const uint8_t *text, size_t size) {
    for (size_t i = 0; i < size;) {
        uint8_t first = text[i++];
        if (!first) return false;
        if (first < 0x80) continue;
        unsigned count;
        uint32_t value, minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1; value = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2; value = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3; value = first & 0x07; minimum = 0x10000;
        } else return false;
        if (count > size - i) return false;
        while (count--) {
            uint8_t next = text[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff
                || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

ssize_t
sc_device_msg_deserialize(const uint8_t *buf, size_t len,
                          struct sc_device_msg *msg) {
    if (!len) {
        return 0; // no message
    }

    msg->type = buf[0];
    switch (msg->type) {
        case DEVICE_MSG_TYPE_CLIPBOARD: {
            if (len < 5) {
                // at least type + empty string length
                return 0; // no complete message
            }
            size_t clipboard_len = sc_read32be(&buf[1]);
            if (clipboard_len > len - 5) {
                return 0; // no complete message
            }
            char *text = malloc(clipboard_len + 1);
            if (!text) {
                LOG_OOM();
                return -1;
            }
            if (clipboard_len) {
                memcpy(text, &buf[5], clipboard_len);
            }
            text[clipboard_len] = '\0';

            msg->clipboard.text = text;
            return 5 + clipboard_len;
        }
        case DEVICE_MSG_TYPE_ACK_CLIPBOARD: {
            if (len < 9) {
                return 0; // no complete message
            }
            uint64_t sequence = sc_read64be(&buf[1]);
            msg->ack_clipboard.sequence = sequence;
            return 9;
        }
        case DEVICE_MSG_TYPE_UHID_OUTPUT: {
            if (len < 5) {
                // at least id + size
                return 0; // not available
            }
            uint16_t id = sc_read16be(&buf[1]);
            size_t size = sc_read16be(&buf[3]);
            if (size > len - 5) {
                return 0; // not available
            }
            uint8_t *data = malloc(size);
            if (!data) {
                LOG_OOM();
                return -1;
            }
            if (size) {
                memcpy(data, &buf[5], size);
            }

            msg->uhid_output.id = id;
            msg->uhid_output.size = size;
            msg->uhid_output.data = data;

            return 5 + size;
        }
        case DEVICE_MSG_TYPE_MIC_STATUS: {
            if (len < 8) return 0;
            uint8_t status = buf[5];
            size_t size = sc_read16be(&buf[6]);
            if (status > SC_MIC_STATUS_ERROR || size > SC_MIC_STATUS_TEXT_MAX_LENGTH)
                return -1;
            if (size > len - 8) return 0;
            if (!valid_status_utf8(&buf[8], size)) return -1;
            msg->mic_status.session = sc_read32be(&buf[1]);
            msg->mic_status.status = status;
            memcpy(msg->mic_status.text, &buf[8], size);
            msg->mic_status.text[size] = '\0';
            return 8 + size;
        }
        case DEVICE_MSG_TYPE_MIC_PCM: {
            if (len < 15) return 0;
            uint64_t pts = sc_read64be(&buf[5]);
            size_t size = sc_read16be(&buf[13]);
            if (pts > INT64_MAX || !size || size > SC_MIC_PCM_MAX_SIZE || size % 2)
                return -1;
            if (size > len - 15) return 0;
            msg->mic_pcm.session = sc_read32be(&buf[1]);
            msg->mic_pcm.pts = (int64_t) pts;
            msg->mic_pcm.size = size;
            memcpy(msg->mic_pcm.data, &buf[15], size);
            return 15 + size;
        }
        case DEVICE_MSG_TYPE_CLOCK: {
            if (len < 17) return 0;
            uint64_t device_us = sc_read64be(&buf[9]);
            if (device_us > INT64_MAX) return -1;
            msg->clock.session = sc_read32be(&buf[1]);
            msg->clock.sequence = sc_read32be(&buf[5]);
            msg->clock.device_us = (int64_t) device_us;
            return 17;
        }
        default:
            LOGW("Unknown device message type: %d", (int) msg->type);
            return -1; // error, we cannot recover
    }
}

void
sc_device_msg_destroy(struct sc_device_msg *msg) {
    switch (msg->type) {
        case DEVICE_MSG_TYPE_CLIPBOARD:
            free(msg->clipboard.text);
            break;
        case DEVICE_MSG_TYPE_UHID_OUTPUT:
            free(msg->uhid_output.data);
            break;
        default:
            // nothing to do
            break;
    }
}
