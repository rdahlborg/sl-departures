#pragma once
#include <stdint.h>

/* Build the departure board. Call with the LVGL port lock held, as for
 * every function here. */
void ui_init(void);

/* Full-screen picker. `options` is newline separated. */
void ui_picker_show(const char *title, const char *options, int selected, uint32_t color);
void ui_picker_select(int index);
void ui_picker_hide(void);
