#ifndef SC_RECEIVER_H
#define SC_RECEIVER_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

#include "uhid/uhid_output.h"
#include "util/acksync.h"
#include "util/net.h"
#include "util/thread.h"

// Invoked directly on the receiver thread. Implementations must be nonblocking;
// strings/PCM are borrowed only for the duration of the callback. The callback
// table and userdata must outlive sc_receiver_join().
struct sc_receiver_microphone_callbacks {
    void (*on_status)(uint32_t session, uint8_t status, const char *message,
                      void *userdata);
    void (*on_pcm)(uint32_t session, int64_t pts, const uint8_t *pcm,
                   uint16_t size, void *userdata);
    void (*on_clock)(uint32_t session, uint32_t sequence, int64_t device_us,
                     void *userdata);
    void (*on_closed)(bool error, void *userdata);
};

// receive events from the device
// managed by the controller
struct sc_receiver {
    sc_socket control_socket;
    sc_thread thread;
    sc_mutex mutex;

    struct sc_acksync *acksync;
    struct sc_uhid_devices *uhid_devices;

    const struct sc_receiver_microphone_callbacks *microphone_cbs;
    void *microphone_userdata;

    const struct sc_receiver_callbacks *cbs;
    void *cbs_userdata;
};

struct sc_receiver_callbacks {
    void (*on_ended)(struct sc_receiver *receiver, bool error, void *userdata);
};

bool
sc_receiver_init(struct sc_receiver *receiver, sc_socket control_socket,
                 const struct sc_receiver_callbacks *cbs, void *cbs_userdata);

void
sc_receiver_destroy(struct sc_receiver *receiver);

// Configure once after init and before start. NULL disables all microphone
// delivery without changing existing clipboard/HID/control handling.
void
sc_receiver_set_microphone_callbacks(struct sc_receiver *receiver,
    const struct sc_receiver_microphone_callbacks *callbacks, void *userdata);

bool
sc_receiver_start(struct sc_receiver *receiver);

// no sc_receiver_stop(), it will automatically stop on control_socket shutdown

void
sc_receiver_join(struct sc_receiver *receiver);

#endif
