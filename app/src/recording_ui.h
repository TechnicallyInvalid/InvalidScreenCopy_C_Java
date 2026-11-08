#ifndef SC_RECORDING_UI_H
#define SC_RECORDING_UI_H
#include "recording.h"
#include "controller.h"
#include <SDL3/SDL.h>

void *sc_recording_ui_create(SDL_Window *window, struct sc_recording *recording,
                              struct sc_controller *controller);
void sc_recording_ui_destroy(void *ui);
#endif
