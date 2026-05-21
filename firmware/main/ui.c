#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "lvgl.h"

#include "pump_task.h"

static const char *TAG = "ui";

#define STATUS_ROW_COUNT     7
#define DEFAULT_SYRINGE_UL   125
#define DEFAULT_STROKE_STEPS 12000

/* Banner / tabview */
static lv_obj_t *s_banner;
static lv_obj_t *s_tabview;

/* Valve tab */
static lv_obj_t *s_valve_buttons[4];
static int s_active_valve_port = -1;

/* Move tab */
static lv_obj_t *s_move_slider;
static lv_obj_t *s_move_target_label;
static lv_obj_t *s_move_current_label;
static lv_obj_t *s_move_aspirate_btn;
static lv_obj_t *s_move_dispense_btn;

/* Prime tab */
static lv_obj_t *s_prime_btn;
static lv_obj_t *s_prime_spinner;
static lv_obj_t *s_prime_label;

/* Status tab */
static lv_obj_t *s_status_table;
static const char *STATUS_ROW_NAMES[STATUS_ROW_COUNT] = {
    "Supply V",   "Valve port", "Plunger steps", "Pump busy",
    "Pump error", "Firmware",   "WiFi",
};

/* Active error modal (NULL when none). */
static lv_obj_t *s_modal;

/* ----------------------------------------------------------------- Helpers */
static void enqueue_or_toast(const pump_cmd_t *cmd, const char *busy_msg)
{
    if (!pump_task_enqueue(cmd)) {
        ui_show_toast(busy_msg);
    }
}

/* ----------------------------------------------------------------- Valve */
static void valve_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    int port = (int)(intptr_t)lv_event_get_user_data(e);
    pump_cmd_t cmd = {
        .kind = PUMP_CMD_VALVE,
        .payload.valve = {.port = port, .ccw = false},
    };
    enqueue_or_toast(&cmd, "Pump busy");
}

static void create_valve_tab(lv_obj_t *parent)
{
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1),
                                   LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1),
                                   LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(parent, col_dsc, row_dsc);
    lv_obj_set_layout(parent, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_all(parent, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(parent, 8, LV_PART_MAIN);

    for (int i = 0; i < 4; ++i) {
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, i % 2, 1,
                             LV_GRID_ALIGN_STRETCH, i / 2, 1);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text_fmt(label, "Port %d", i + 1);
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, valve_btn_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(i + 1));
        s_valve_buttons[i] = btn;
    }
}

static void valve_highlight_port(int port)
{
    s_active_valve_port = port;
    for (int i = 0; i < 4; ++i) {
        if (s_valve_buttons[i] == NULL) {
            continue;
        }
        lv_color_t bg = (i + 1 == port) ? lv_palette_main(LV_PALETTE_BLUE)
                                        : lv_palette_main(LV_PALETTE_GREY);
        lv_obj_set_style_bg_color(s_valve_buttons[i], bg, LV_PART_MAIN);
    }
}

/* ----------------------------------------------------------------- Move */
static int slider_target_uL(void)
{
    if (s_move_slider == NULL) {
        return 0;
    }
    return (int)lv_slider_get_value(s_move_slider);
}

static void move_slider_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_move_target_label != NULL) {
        lv_label_set_text_fmt(s_move_target_label, "Target: %d µL",
                              slider_target_uL());
    }
}

static void aspirate_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    pump_cmd_t cmd = {
        .kind = PUMP_CMD_ASPIRATE,
        .payload.volume = {.target_uL = (float)slider_target_uL()},
    };
    enqueue_or_toast(&cmd, "Pump busy");
}

static void dispense_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    pump_cmd_t cmd = {
        .kind = PUMP_CMD_DISPENSE,
        .payload.volume = {.target_uL = (float)slider_target_uL()},
    };
    enqueue_or_toast(&cmd, "Pump busy");
}

static void create_move_tab(lv_obj_t *parent)
{
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(parent, 6, LV_PART_MAIN);

    s_move_target_label = lv_label_create(parent);
    lv_label_set_text(s_move_target_label, "Target: 0 µL");

    s_move_slider = lv_slider_create(parent);
    lv_slider_set_range(s_move_slider, 0, DEFAULT_SYRINGE_UL);
    lv_slider_set_value(s_move_slider, 0, LV_ANIM_OFF);
    lv_obj_set_width(s_move_slider, LV_PCT(95));
    lv_obj_add_event_cb(s_move_slider, move_slider_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_move_current_label = lv_label_create(parent);
    lv_label_set_text(s_move_current_label, "Current: -- µL");

    lv_obj_t *btn_row = lv_obj_create(parent);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_size(btn_row, LV_PCT(100), 56);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_move_aspirate_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_move_aspirate_btn, 130, 48);
    lv_obj_t *al = lv_label_create(s_move_aspirate_btn);
    lv_label_set_text(al, "Aspirate to");
    lv_obj_center(al);
    lv_obj_add_event_cb(s_move_aspirate_btn, aspirate_btn_event_cb,
                        LV_EVENT_CLICKED, NULL);

    s_move_dispense_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_move_dispense_btn, 130, 48);
    lv_obj_t *dl = lv_label_create(s_move_dispense_btn);
    lv_label_set_text(dl, "Dispense to");
    lv_obj_center(dl);
    lv_obj_add_event_cb(s_move_dispense_btn, dispense_btn_event_cb,
                        LV_EVENT_CLICKED, NULL);
}

/* ----------------------------------------------------------------- Prime */
static void prime_confirm_cb(lv_event_t *e)
{
    lv_obj_t *modal = lv_event_get_current_target(e);
    const char *txt = lv_msgbox_get_active_btn_text(modal);
    if (txt != NULL && strcmp(txt, "Start") == 0) {
        pump_cmd_t cmd = {
            .kind = PUMP_CMD_PRIME,
            .payload.prime = {.cycles = 1, .source_port = 3, .sink_port = 1},
        };
        enqueue_or_toast(&cmd, "Pump busy");
    }
    lv_msgbox_close(modal);
}

static void prime_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    static const char *btns[] = {"Start", "Cancel", ""};
    lv_obj_t *modal = lv_msgbox_create(NULL, "Prime line",
                                       "Run one prime cycle:\n"
                                       "port 3 (source) → port 1 (sink)\n"
                                       "Full-stroke aspirate + dispense.",
                                       btns, false);
    lv_obj_add_event_cb(modal, prime_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(modal);
}

static void create_prime_tab(lv_obj_t *parent)
{
    s_prime_btn = lv_btn_create(parent);
    lv_obj_set_size(s_prime_btn, 240, 100);
    lv_obj_align(s_prime_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *label = lv_label_create(s_prime_btn);
    lv_label_set_text(label, "PRIME\nport 3 → port 1");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_add_event_cb(s_prime_btn, prime_btn_event_cb, LV_EVENT_CLICKED,
                        NULL);

    s_prime_spinner = lv_spinner_create(parent, 1000, 60);
    lv_obj_set_size(s_prime_spinner, 80, 80);
    lv_obj_align(s_prime_spinner, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(s_prime_spinner, LV_OBJ_FLAG_HIDDEN);

    s_prime_label = lv_label_create(parent);
    lv_label_set_text(s_prime_label, "");
    lv_obj_align(s_prime_label, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void prime_set_running(bool running)
{
    if (s_prime_btn == NULL) {
        return;
    }
    if (running) {
        lv_obj_add_flag(s_prime_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_prime_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_prime_label, "Priming (~30 s) ...");
    } else {
        lv_obj_clear_flag(s_prime_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_prime_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ----------------------------------------------------------------- Status */
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

/* ----------------------------------------------------------------- Motion
 * enable */
static void set_disabled(lv_obj_t *obj, bool disabled)
{
    if (obj == NULL) {
        return;
    }
    if (disabled) {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(obj, LV_STATE_DISABLED);
    }
}

static void apply_motion_enabled(bool enabled)
{
    for (int i = 0; i < 4; ++i) {
        set_disabled(s_valve_buttons[i], !enabled);
    }
    set_disabled(s_move_slider, !enabled);
    set_disabled(s_move_aspirate_btn, !enabled);
    set_disabled(s_move_dispense_btn, !enabled);
    set_disabled(s_prime_btn, !enabled);
}

/* ----------------------------------------------------------------- ui_create
 */
void ui_create(void)
{
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

    create_valve_tab(valve_tab);
    create_move_tab(move_tab);
    create_prime_tab(prime_tab);
    create_status_tab(status_tab);

    lv_tabview_set_act(s_tabview, 3, LV_ANIM_OFF);
    apply_motion_enabled(false);

    ESP_LOGI(TAG, "UI created (full 4-tab motion UI)");
}

/* -----------------------------------------------------------------
 * ui_apply_state */
void ui_apply_state(app_state_t state, const char *banner_text,
                    bool requires_reinit)
{
    if (s_banner == NULL) {
        return;
    }
    lv_color_t color;
    bool motion_ok = false;
    switch (state) {
    case APP_STATE_BOOT:
    case APP_STATE_WIFI_CONNECTING:
    case APP_STATE_DIAGNOSING:
        color = lv_palette_main(LV_PALETTE_BLUE_GREY);
        break;
    case APP_STATE_NEEDS_INIT:
        color = lv_palette_main(LV_PALETTE_AMBER);
        break;
    case APP_STATE_READY:
        color = lv_palette_main(LV_PALETTE_GREEN);
        motion_ok = !requires_reinit;
        break;
    case APP_STATE_BUSY:
        color = lv_palette_main(LV_PALETTE_INDIGO);
        break;
    case APP_STATE_WIFI_LOST:
    case APP_STATE_ERROR_RECOVERABLE:
        color = lv_palette_main(LV_PALETTE_RED);
        break;
    case APP_STATE_ERROR_FATAL:
        color = lv_palette_darken(LV_PALETTE_RED, 2);
        break;
    default:
        color = lv_palette_main(LV_PALETTE_GREY);
        break;
    }
    lv_obj_set_style_bg_color(s_banner, color, LV_PART_MAIN);
    if (banner_text != NULL) {
        lv_label_set_text(s_banner, banner_text);
    }

    apply_motion_enabled(motion_ok);
    prime_set_running(state == APP_STATE_BUSY);
}

/* -----------------------------------------------------------------
 * ui_apply_status */
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

void ui_apply_motion_snapshot(const app_status_t *status)
{
    if (status == NULL) {
        return;
    }
    /* Valve highlight from the cached server-reported port. */
    if (status->valve[0] >= '1' && status->valve[0] <= '4' &&
        status->valve[1] == '\0') {
        valve_highlight_port(status->valve[0] - '0');
    } else {
        valve_highlight_port(-1);
    }
    /* Move tab — Current label converted back to µL. */
    if (s_move_current_label != NULL) {
        float uL = (float)status->plunger_steps * (float)DEFAULT_SYRINGE_UL /
                   (float)DEFAULT_STROKE_STEPS;
        lv_label_set_text_fmt(s_move_current_label, "Current: %.1f µL", uL);
    }
}

/* ----------------------------------------------------------------- Error modal
 */
static void modal_event_cb(lv_event_t *e)
{
    lv_obj_t *modal = lv_event_get_current_target(e);
    const char *txt = lv_msgbox_get_active_btn_text(modal);
    if (txt == NULL) {
        lv_msgbox_close(modal);
        s_modal = NULL;
        return;
    }
    if (strcmp(txt, "Retry") == 0) {
        pump_cmd_t cmd = {.kind = PUMP_CMD_RETRY_LAST};
        enqueue_or_toast(&cmd, "Pump busy");
    } else if (strcmp(txt, "Re-initialize") == 0) {
        pump_cmd_t cmd = {
            .kind = PUMP_CMD_INITIALIZE,
            .payload.init = {.force = 2, .ccw = false},
        };
        enqueue_or_toast(&cmd, "Pump busy");
    }
    lv_msgbox_close(modal);
    s_modal = NULL;
}

void ui_show_error_modal(const pump_error_t *err)
{
    if (err == NULL || err->error_name[0] == '\0') {
        return;
    }
    if (s_modal != NULL) {
        /* Keep the existing modal — later errors get logged but the
         * first one is what the operator should address. */
        ESP_LOGW(TAG, "modal already open, dropping: %s", err->error_name);
        return;
    }
    static const char *recover_btns[] = {"Retry", "Dismiss", ""};
    static const char *fatal_btns[] = {"Re-initialize", ""};
    const char **btns =
        (err->klass == PUMP_ERROR_FATAL) ? fatal_btns : recover_btns;

    char body[280];
    snprintf(body, sizeof(body), "%s (code %d, HTTP %d)\n%s\n%s%s%s",
             err->error_name, err->code, err->http_status, err->message,
             err->command[0] != '\0' ? "cmd " : "",
             err->command[0] != '\0' ? err->command : "",
             err->command[0] != '\0' ? "\n" : "");

    s_modal = lv_msgbox_create(
        NULL,
        err->klass == PUMP_ERROR_FATAL ? "Fatal pump error" : "Pump error",
        body, btns, false);
    lv_obj_add_event_cb(s_modal, modal_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(s_modal);
}

/* ----------------------------------------------------------------- Toast */
static void toast_timer_cb(lv_timer_t *t)
{
    lv_obj_t *toast = (lv_obj_t *)lv_timer_get_user_data(t);
    if (toast != NULL) {
        lv_obj_del(toast);
    }
    lv_timer_del(t);
}

void ui_show_toast(const char *msg)
{
    if (msg == NULL) {
        return;
    }
    lv_obj_t *toast = lv_label_create(lv_scr_act());
    lv_label_set_text(toast, msg);
    lv_obj_set_style_bg_color(toast, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toast, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_text_color(toast, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(toast, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(toast, 6, LV_PART_MAIN);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_move_foreground(toast);
    lv_timer_t *t = lv_timer_create(toast_timer_cb, 2000, toast);
    lv_timer_set_repeat_count(t, 1);
}

void ui_jump_to_status_tab(void)
{
    if (s_tabview != NULL) {
        lv_tabview_set_act(s_tabview, 3, LV_ANIM_OFF);
    }
}
