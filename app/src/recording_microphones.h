#ifndef SC_RECORDING_MICROPHONES_H
#define SC_RECORDING_MICROPHONES_H

#include <stdbool.h>
struct sc_controller;
struct sc_recording;
struct sc_recording_microphones;

// Allocate/configure before the controller starts. This opens no capture device
// and requests no permission. Destroy after recording and controller join.
struct sc_recording_microphones *
sc_recording_microphones_create(struct sc_recording *recording,
                                struct sc_controller *controller,
                                bool device_output);
void sc_recording_microphones_destroy(struct sc_recording_microphones *backend);
#endif
