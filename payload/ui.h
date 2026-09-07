// ui.h — the screen: a PLAY readout and a five-page editor.
#pragma once

#include <stdint.h>

#define PAGE_VOICE 0
#define PAGE_ENV   1
#define PAGE_SWEEP 2
#define PAGE_MAP   3
#define PAGE_CAL   4
#define PAGE_COUNT 5

void ui_init(void);
void ui_frame(void);      // one frame: handle buttons, redraw what changed

extern uint8_t g_page;
