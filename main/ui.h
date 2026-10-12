#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Build the departure board. Call with the LVGL port lock held, as for
 * every function here. */
void ui_init(void);

/* Full-screen picker. `options` is newline separated. */
void ui_picker_show(const char *title, const char *options, int selected, uint32_t color);
void ui_picker_select(int index);
void ui_picker_hide(void);

/* QR code for the settings page on the home network. It closes itself
 * when a station is chosen from the phone. */
void ui_phone_show(void);
void ui_phone_hide(void);
bool ui_phone_visible(void);

/* Version, address and update status. */
void ui_about_show(void);
void ui_about_hide(void);
bool ui_about_visible(void);

/* Debug, for recording the docs' animations: show the boot screen with the
 * train still at the given frame of a run of UI_TRAIN_FRAMES (frame
 * UI_TRAIN_FRAMES is the pause between runs). Negative closes it. */
#define UI_TRAIN_FRAMES 20
void ui_boot_demo(int frame);
