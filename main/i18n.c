/* Display language, see i18n.h. Saved next to the station (see controls.c). */
#include "nvs.h"
#include "i18n.h"

#define NVS_NS  "sl"

static volatile lang_t lang = LANG_SV;

void i18n_load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "lang", &v) == ESP_OK && v <= LANG_EN) {
            lang = v;
        }
        nvs_close(h);
    }
}

lang_t i18n_lang(void)
{
    return lang;
}

void i18n_set(lang_t l)
{
    lang = l;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "lang", l);
        nvs_commit(h);
        nvs_close(h);
    }
}
