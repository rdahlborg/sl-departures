#pragma once

/* Redraw the screen and print it as "SNAP:" lines of hex RGB565, which
 * tools/screenshot.py turns into a PNG. Triggered by 'S' on USB serial. */
void screenshot_dump(void);
