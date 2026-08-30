#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "encoder.h"
#include "gfx.h"

typedef enum {
    APP_UI_MAIN,
    APP_UI_MENU,
    APP_UI_DETAIL,
} app_ui_page_t;

typedef enum {
    APP_UI_MENU_SERVERS,
    APP_UI_MENU_RADIO,
    APP_UI_MENU_AUDIO,
    APP_UI_MENU_APRS,
    APP_UI_MENU_ESPNOW,
    APP_UI_MENU_NET_RADIO,
    APP_UI_MENU_OTA,
    APP_UI_MENU_LANGUAGE,
    APP_UI_MENU_COUNT,
} app_ui_menu_t;

typedef struct {
    app_ui_page_t page;
    size_t server_index;
    size_t fmo_favorite_index;
    size_t fmo_server_index;
    uint8_t server_select_mode; /* 0=choose direction, 1=NRL, 2=FMO */
    int menu_index;
    bool server_change_pending;
    bool fmo_server_change_pending;
    int64_t server_preview_until_us;  /* show server scroll list until this time */
    char callsign[16];
    uint8_t callsign_ssid;
    float tx_mhz;
    float rx_mhz;
    float tx_ctcss_hz;
    float rx_ctcss_hz;
    bool radio_present;
    bool codec_present;
    bool chinese;
    bool language_change_pending;
    bool espnow_enabled;
    bool espnow_change_pending;
    bool aprs_enabled;
    bool aprs_change_pending;
    uint8_t radio_adjust_field;  /* RF detail selected field (0-9) */
    uint8_t audio_adjust_field;  /* audio detail selected field (0-6) */
    /* 0=APRS-IS, 1=RF RX, 2=RF TX, 3=NRL RX, 4=NRL TX,
     * 5=RF>IS, 6=IS>RF, 7=NRL>IS, 8=IS>NRL, 9=RF>NRL, 10=NRL>RF */
    uint8_t aprs_adjust_field;
    bool ota_install_confirm;
} app_ui_t;

void app_ui_init(app_ui_t *ui);
void app_ui_handle(app_ui_t *ui, encoder_event_type_t event);
/* Flushes deferred screen edits after the knob has been idle.  Returns true
 * when the save indicator changed and the screen should be redrawn. */
bool app_ui_poll(void);
void app_ui_render(const app_ui_t *ui, gfx_canvas_t *canvas);
