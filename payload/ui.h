// ui.h — the screen: a PLAY readout and a paged editor.
#pragma once

#include <stdint.h>

// Page order is the order they appear on the tab bar, and it runs roughly in the order you
// build a sound: what the channels are, what triggers them, how they move, how they balance,
// then the mappings, then the housekeeping.
#define PAGE_CHAN  0
#define PAGE_TRIG  1
#define PAGE_ENV   2
#define PAGE_MIX   3
#define PAGE_BTN   4
#define PAGE_MAP   5
#define PAGE_ORN   6
#define PAGE_DRUM  7
#define PAGE_MEM   8
#define PAGE_CAL   9
#define PAGE_SET   10
#define PAGE_COUNT 11

void ui_init(void);
void ui_frame(void);      // one frame: handle buttons, redraw what changed

extern uint8_t g_page;
