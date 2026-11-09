#ifndef SC_RECORDING_UI_H
#define SC_RECORDING_UI_H
#include "recording.h"
#include "controller.h"
#include <SDL3/SDL.h>

struct sc_screen;
void *sc_recording_ui_create(struct sc_screen *screen, struct sc_recording *recording,
                              struct sc_controller *controller);
void sc_recording_ui_destroy(void *ui);
#endif
