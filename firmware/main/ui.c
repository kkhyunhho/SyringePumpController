#include "ui.h"

#include <stdio.h>

#include "esp_log.h"

#include "lvgl.h"

static const char *TAG = "ui";

#define STATUS_ROW_COUNT 7

static lv_obj_t *s_banner;
static lv_obj_t *s_tabview;
static lv_obj_t *s_status_table;
static lv_obj_t *s_placeholder_labels[3];

static const char *STATUS_ROW_NAMES[STATUS_ROW_COUNT] = {
    "Supply V",   "Valve port", "Plunger steps", "Pump busy",
    "Pump error", "Firmware",   "WiFi",
};

static void create_status_tab(lv_obj_t *parent)
{
    s_status_table = lv_table_create(parent);
    lv_obj_set_size(s_status_table, LV_PCT(100), LV_PCT(100));
    lv_table_set_row_cnt(s_status_table, STATUS_ROW_COUNT);
    lv_table_set_col_cnt(s_status_table, 2);
    lv_table_set_col_width(s_status_table, 0, 130);
    lv_table_set_col_width(s_status_table, 1, 180);
    for (int i = 0; i < STATUS_ROW_COUNT; ++i) {
        lv_table_set_cell_value(s_status_table, (uint16_t)i, 0,
                                STATUS_ROW_NAMES[i]);
        lv_table_set_cell_value(s_status_table, (uint16_t)i, 1, "--");
    }
}

static void create_placeholder_tab(lv_obj_t *parent, int idx, const char *name)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text_fmt(label, "%s — Phase C (not yet wired)", name);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_GREY),
                                LV_PART_MAIN);
    s_placeholder_labels[idx] = label;
}

void ui_create(void)
{
    /* Top banner: 24 px high above the tabview. */
    s_banner = lv_label_create(lv_scr_act());
    lv_obj_set_size(s_banner, LV_PCT(100), 24);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(s_banner, "Boot");
    lv_obj_set_style_bg_color(s_banner, lv_palette_main(LV_PALETTE_BLUE_GREY),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_banner, lv_color_white(), LV_PART_MAIN);

    s_tabview = lv_tabview_create(lv_scr_act());
    lv_tabview_set_tab_bar_size(s_tabview, 32);
    lv_obj_set_size(s_tabview, LV_PCT(100), 216);
    lv_obj_align(s_tabview, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *valve_tab = lv_tabview_add_tab(s_tabview, "Valve");
    lv_obj_t *move_tab = lv_tabview_add_tab(s_tabview, "Move");
    lv_obj_t *prime_tab = lv_tabview_add_tab(s_tabview, "Prime");
    lv_obj_t *status_tab = lv_tabview_add_tab(s_tabview, "Status");

    create_placeholder_tab(valve_tab, 0, "Valve");
    create_placeholder_tab(move_tab, 1, "Move");
    create_placeholder_tab(prime_tab, 2, "Prime");
    create_status_tab(status_tab);

    /* Phase B starts on the Status tab (the only live one). */
    lv_tabview_set_act(s_tabview, 3, LV_ANIM_OFF);

    ESP_LOGI(TAG, "UI created (Status tab live, others Phase C)");
}

void ui_apply_state(app_state_t state, const char *banner_text)
{
    if (s_banner == NULL) {
        return;
    }
    lv_color_t color;
    switch (state) {
    case APP_STATE_BOOT:
    case APP_STATE_WIFI_CONNECTING:
    case APP_STATE_DIAGNOSING:
        color = lv_palette_main(LV_PALETTE_BLUE_GREY);
        break;
    case APP_STATE_NEEDS_INIT:
        color = lv_palette_main(LV_PALETTE_AMBER);
        break;
    case APP_STATE_WIFI_LOST:
    case APP_STATE_ERROR_RECOVERABLE:
        color = lv_palette_main(LV_PALETTE_RED);
        break;
    default:
        color = lv_palette_main(LV_PALETTE_GREY);
        break;
    }
    lv_obj_set_style_bg_color(s_banner, color, LV_PART_MAIN);
    if (banner_text != NULL) {
        lv_label_set_text(s_banner, banner_text);
    }
}

void ui_apply_status(const app_status_t *status)
{
    if (s_status_table == NULL || status == NULL) {
        return;
    }
    char buf[64];

    snprintf(buf, sizeof(buf), "%.1f V", status->supply_volts);
    lv_table_set_cell_value(s_status_table, 0, 1, buf);

    lv_table_set_cell_value(s_status_table, 1, 1, status->valve);

    snprintf(buf, sizeof(buf), "%d", status->plunger_steps);
    lv_table_set_cell_value(s_status_table, 2, 1, buf);

    lv_table_set_cell_value(s_status_table, 3, 1,
                            status->pump_busy ? "yes" : "no");

    snprintf(buf, sizeof(buf), "%s (%d)", status->pump_error_name,
             status->pump_error_code);
    lv_table_set_cell_value(s_status_table, 4, 1, buf);

    lv_table_set_cell_value(s_status_table, 5, 1, status->software_version);

    lv_table_set_cell_value(s_status_table, 6, 1,
                            status->wifi_connected ? "connected" : "lost");
}
