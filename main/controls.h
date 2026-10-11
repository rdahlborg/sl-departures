#pragma once
#include <stdint.h>

/* Load the saved station (or the default) into *group / *site. */
void controls_load(int *group, uint16_t *site);

/* Show this station and remember it. Safe from any task. */
void controls_select(int group, uint16_t site);

/* Start listening to the BOOT button. */
void controls_init(void);

/* Simulate a button gesture: 'n' click, 'p' double click, 'h' hold. */
void controls_simulate(char gesture);
