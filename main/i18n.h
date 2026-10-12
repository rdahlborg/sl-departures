#pragma once
#include <stdbool.h>

/* Swedish (the default) or English, for the screen and the web pages.
 * Station and destination names come from SL and stay as they are. */
typedef enum { LANG_SV, LANG_EN } lang_t;

/* Read the saved choice. Call after NVS is initialised. */
void i18n_load(void);
lang_t i18n_lang(void);
/* Switch and save. */
void i18n_set(lang_t lang);

/* The text for the current language: T("Välj linje", "Choose line"). */
#define T(sv, en) (i18n_lang() == LANG_EN ? (en) : (sv))
