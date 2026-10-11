#pragma once

/* USB serial input: Improv Wi-Fi packets from the web installer, plus
 * single-key debug commands:
 *   S   dump a screenshot (tools/screenshot.py)
 *   n   simulate a button click, p double click, h hold */
void serial_init(void);

/* Enable the debug keys; until then only Improv is handled. */
void serial_ui_ready(void);
