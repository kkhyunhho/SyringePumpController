#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "lvgl.h"

#include "pump_task.h"

static const char *TAG = "ui";

/* Custom Montserrat-14 font with ASCII (0x20-0x7E) + micro sign (0xB5)
 * + a few arrows / em-dash / ellipsis. Generated via the LVGL Font
 * Converter from Montserrat-Regular.ttf and checked in as
 * ``firmware/main/montserrat_14_ext.c``. Needed because the built-in
 * lv_font_montserrat_14 lacks U+00B5 (µ) and friends, while the custom
 * subset can't economically cover every glyph we might ever want.
 *
 * ``s_font_main`` below is a mutable runtime wrapper around the
 * generated const font with ``fallback = &lv_font_montserrat_14`` set:
 * every label / theme reference uses ``&s_font_main``, so any glyph
 * missing from our subset (e.g. FontAwesome icons like LV_SYMBOL_DOWN
 * for the dropdown caret) is rendered cleanly from LVGL's built-in
 * Montserrat-14 instead of becoming a missing-glyph box. */
LV_FONT_DECLARE(montserrat_14_ext);
LV_FONT_DECLARE(lv_font_montserrat_14);
static lv_font_t s_font_main;

#define STATUS_ROW_COUNT 7
/* Fallback used only before the first diagnose response lands; the
 * slider gets re-ranged from app_status_t.syringe_uL as soon as
 * /v1/diagnose succeeds. Matches the existing bench syringe so a
 * boot-time aspirate (theoretically possible before diagnose, though
 * the FSM gates motion behind READY) doesn't over-request. */
#define DEFAULT_SYRINGE_UL 125

/* Banner / tabview */
static lv_obj_t *s_banner;
static lv_obj_t *s_tabview;

/* Valve tab */
static lv_obj_t *s_valve_buttons[4];
static int s_active_valve_port = -1;
/* Rotor diagram overlaid in the centre of the 2x2 port-button grid.
 * Two coloured chords show the valve state. The *named* port (1 or 3)
 * is always tied to the common C; that chord carries the Path colour
 * (Path 1 = BLUE, Path 2 = ORANGE). The remaining two ports are
 * bridged (bypass) and drawn in the other Path's colour. Per-position
 * chords set by ``valve_diagram_render``:
 *   pos 1 (Port 1 → Path 1): BLUE  C↔1, ORANGE 2↔3
 *   pos 2 (Port 3 → Path 1): BLUE  C↔3, ORANGE 1↔2
 *   pos 3 (Port 1 → Path 2): ORANGE C↔1, BLUE 2↔3
 *   pos 4 (Port 3 → Path 2): ORANGE C↔3, BLUE 1↔2
 * Colour is therefore set at render time, not creation time: the same
 * geometric chord (e.g. C↔1) is blue under pos 1 but orange under pos 3.
 */
static lv_obj_t *s_valve_diagram;
static lv_obj_t *s_valve_line_c_p1;  /* chord C↔Port 1 */
static lv_obj_t *s_valve_line_c_p3;  /* chord C↔Port 3 */
static lv_obj_t *s_valve_line_p2_p3; /* chord Port 2↔Port 3 */
static lv_obj_t *s_valve_line_p1_p2; /* chord Port 1↔Port 2 */

/* Move tab */
static lv_obj_t *s_move_slider;
static lv_obj_t *s_move_target_label;
static lv_obj_t *s_move_valve_label;   /* "Connected: Port N" */
static lv_obj_t *s_move_actuate_btn;   /* unified "C Actuation" button */
static lv_obj_t *s_move_history_label; /* "Last: ..." */

/* Status tab — reconnect button to retry diagnose after a server outage. */
static lv_obj_t *s_reconnect_btn;

/* Aspirate/Dispense tab — operator-configurable cycles / source / sink /
 * per-cycle volume. Source and sink select a full *valve position*
 * (1-4, the ``Port n to Path m`` set shared with the Valve tab) rather
 * than just a leading port number: positions 1 and 3 are both physical
 * Port 1, so the old "Port 1 / Port 3" dropdown actually drove both
 * source and sink onto physical Port 1 (fluid came back out the source).
 * The volume slider mirrors the Move tab's Target bar and sets how much
 * each cycle aspirates from the source before dispensing it to the
 * sink. ``s_prime_source`` / ``s_prime_sink`` hold valve positions. */
static lv_obj_t *s_prime_btn;
static lv_obj_t *s_prime_btn_label;
static lv_obj_t *s_prime_spinner;
static lv_obj_t *s_prime_label;
static lv_obj_t *s_prime_cycles_label;
static lv_obj_t *s_prime_source_dd;
static lv_obj_t *s_prime_sink_dd;
static lv_obj_t *s_prime_volume_slider;
static lv_obj_t *s_prime_volume_label;
static int s_prime_cycles = 1;
/* Defaults: source = pos 1 (Port 1 → Path 1, physical Port 1),
 * sink = pos 2 (Port 3 → Path 1, physical Port 3) — a real source→sink
 * pair, unlike the old 1/3 which collapsed to one physical port. */
static int s_prime_source = 1;
static int s_prime_sink = 2;

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
    /* ccw=false → pump sends ``I<port>R`` (clockwise rotation per
     * SY01BE manual §4.5.1). ccw=true would be O<port>R (CCW). Back to
     * CW per bench preference. */
    pump_cmd_t cmd = {
        .kind = PUMP_CMD_VALVE,
        .payload.valve = {.port = port, .ccw = false},
    };
    enqueue_or_toast(&cmd, "Pump busy");
}

/* Rotor diagram is a 64x64 lv_obj with border 2 → 60x60 content area
 * (diameter cut ~1/4 from the former 84 px so the four buttons can
 * tile the whole tab behind it). Child coordinates (chord endpoints,
 * label offsets) live in that content-area space; the center is at
 * (30, 30). Each port endpoint sits 21 px from center along an axis
 * (~70 % of the 30-px radius), leaving a few pixels between chord tip
 * and port label. */
static const lv_point_precise_t VALVE_PTS_C_P1[] = {{30, 51}, {9, 30}};
static const lv_point_precise_t VALVE_PTS_C_P3[] = {{30, 51}, {51, 30}};
static const lv_point_precise_t VALVE_PTS_P2_P3[] = {{30, 9}, {51, 30}};
static const lv_point_precise_t VALVE_PTS_P1_P2[] = {{9, 30}, {30, 9}};

static void valve_diagram_render(int position)
{
    lv_obj_t *chords[4] = {s_valve_line_c_p1, s_valve_line_c_p3,
                           s_valve_line_p2_p3, s_valve_line_p1_p2};
    if (chords[0] == NULL) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        lv_obj_add_flag(chords[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* Unknown valve position (status hasn't landed, or pump reports "?")
     * → leave all chords hidden. Rotor body + port labels stay visible. */
    if (position < 1 || position > 4) {
        return;
    }
    /* The named port (Port 1 for pos 1/3, Port 3 for pos 2/4) ties to C;
     * that chord carries the Path colour. The remaining two ports bridge
     * (bypass) in the other Path's colour. */
    bool port1 = (position == 1 || position == 3);
    bool path1 = (position == 1 || position == 2);
    lv_obj_t *c_chord = port1 ? s_valve_line_c_p1 : s_valve_line_c_p3;
    lv_obj_t *bypass = port1 ? s_valve_line_p2_p3 : s_valve_line_p1_p2;
    lv_color_t c_color =
        lv_palette_main(path1 ? LV_PALETTE_BLUE : LV_PALETTE_ORANGE);
    lv_color_t bypass_color =
        lv_palette_main(path1 ? LV_PALETTE_ORANGE : LV_PALETTE_BLUE);
    lv_obj_set_style_line_color(c_chord, c_color, LV_PART_MAIN);
    lv_obj_set_style_line_color(bypass, bypass_color, LV_PART_MAIN);
    lv_obj_clear_flag(c_chord, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bypass, LV_OBJ_FLAG_HIDDEN);
}

/* One of the two grid rows. Each row grows to fill half the tab height
 * and lays its two buttons out side by side; the buttons themselves
 * grow to fill the row width. The 2x2 result tiles the whole tab. */
static void valve_row_setup(lv_obj_t *row)
{
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(row, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
}

static void valve_add_button(lv_obj_t *row, const char *text, int port_index)
{
    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_flex_grow(btn, 1);       /* fill half the row width */
    lv_obj_set_height(btn, LV_PCT(100)); /* fill the row height */
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, valve_btn_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(port_index + 1));
    s_valve_buttons[port_index] = btn;
}

/* Colour is applied per-position in ``valve_diagram_render`` (the same
 * chord switches palette between Path 1 and Path 2), so creation only
 * sets geometry + stroke. */
static void valve_add_chord(lv_obj_t **out, lv_obj_t *parent,
                            const lv_point_precise_t pts[2])
{
    *out = lv_line_create(parent);
    lv_line_set_points(*out, pts, 2);
    lv_obj_set_style_line_width(*out, 4, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(*out, true, LV_PART_MAIN);
}

/* The valve exposes four Port × Path positions (1-4). Both the Valve
 * tab buttons and the Asp/Disp source/sink selectors choose among them;
 * index i ↔ valve position i+1, which is the ``I<n>`` argument sent to
 * the pump. MCC-4 has 2 mechanical states (C-1+2-3 vs C-3+1-2) that the
 * firmware exposes as these 4 positions. CRUCIAL: positions 1 and 3 are
 * both physical Port 1 (Path 1 vs Path 2), and positions 2 and 4 are
 * both physical Port 3 — so a source/sink pair must differ by *position*
 * here, not just by the leading port number. */
static const char *const VALVE_POSITION_LABELS[4] = {
    "Port 1 to Path 1", /* position 1 */
    "Port 3 to Path 1", /* position 2 */
    "Port 1 to Path 2", /* position 3 */
    "Port 3 to Path 2", /* position 4 */
};

static void create_valve_tab(lv_obj_t *parent)
{
    /* Four buttons tile the whole tab as a 2x2 grid (two flex rows, each
     * growing to half the height; two buttons per row, each growing to
     * half the width). The rotor diagram is overlaid in the centre
     * afterwards, so the buttons fill the formerly-empty space either
     * side of it and nothing runs off the bottom of the screen. */
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(parent, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(parent, LV_DIR_NONE);

    /* Top row: positions 1 and 2 (both Path 1 — syringe-active). */
    lv_obj_t *top_row = lv_obj_create(parent);
    valve_row_setup(top_row);
    valve_add_button(top_row, VALVE_POSITION_LABELS[0], 0);
    valve_add_button(top_row, VALVE_POSITION_LABELS[1], 1);

    /* Bottom row: positions 3 and 4 (both Path 2 — passive). */
    lv_obj_t *bot_row = lv_obj_create(parent);
    valve_row_setup(bot_row);
    valve_add_button(bot_row, VALVE_POSITION_LABELS[2], 2);
    valve_add_button(bot_row, VALVE_POSITION_LABELS[3], 3);

    /* Rotor diagram (64x64) floated over the centre of the grid. It is
     * excluded from the flex flow (IGNORE_LAYOUT) and aligned to the tab
     * centre, then raised to the foreground so it reads as a status
     * "hole" punched through the four buttons. Non-clickable so taps in
     * the centre still reach the button underneath. */
    s_valve_diagram = lv_obj_create(parent);
    lv_obj_add_flag(s_valve_diagram, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(s_valve_diagram, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_valve_diagram, 64, 64);
    lv_obj_align(s_valve_diagram, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_valve_diagram, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_valve_diagram, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_valve_diagram,
                                  lv_palette_main(LV_PALETTE_GREY),
                                  LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_valve_diagram, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_valve_diagram, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_valve_diagram, LV_DIR_NONE);

    /* Port labels at compass positions just inside the rotor body. */
    static const struct {
        const char *txt;
        int x;
        int y;
    } LABELS[] = {
        {"1", -24, 0}, /* left */
        {"2", 0, -24}, /* top */
        {"3", 24, 0},  /* right */
        {"C", 0, 24},  /* bottom */
    };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *l = lv_label_create(s_valve_diagram);
        lv_label_set_text(l, LABELS[i].txt);
        lv_obj_align(l, LV_ALIGN_CENTER, LABELS[i].x, LABELS[i].y);
        lv_obj_set_style_text_color(l,
                                    lv_palette_darken(LV_PALETTE_GREY, 2),
                                    LV_PART_MAIN);
    }

    /* All four chords created upfront; valve_diagram_render toggles
     * visibility and colour per position. */
    valve_add_chord(&s_valve_line_c_p1, s_valve_diagram, VALVE_PTS_C_P1);
    valve_add_chord(&s_valve_line_c_p3, s_valve_diagram, VALVE_PTS_C_P3);
    valve_add_chord(&s_valve_line_p2_p3, s_valve_diagram, VALVE_PTS_P2_P3);
    valve_add_chord(&s_valve_line_p1_p2, s_valve_diagram, VALVE_PTS_P1_P2);
    valve_diagram_render(-1); /* hide all until /v1/status lands */

    lv_obj_move_foreground(s_valve_diagram); /* float over the button grid */
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
    valve_diagram_render(port);
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

static void move_history_set(const char *text)
{
    if (s_move_history_label != NULL) {
        lv_label_set_text(s_move_history_label, text);
    }
}

/* "C Actuation" — single button that moves the plunger to the slider's
 * absolute target volume. Aspirate / Dispense were two buttons sending
 * different command kinds (PUMP_CMD_ASPIRATE vs PUMP_CMD_DISPENSE) but
 * both ultimately resolved to the same wire frame (A<n>R), so a single
 * button is more honest about what the call actually does. */
static void actuate_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    int vol = slider_target_uL();
    pump_cmd_t cmd = {
        .kind = PUMP_CMD_ASPIRATE, /* aspirate/dispense share a wire frame */
        .payload.volume = {.target_uL = (float)vol},
    };
    enqueue_or_toast(&cmd, "Pump busy");
    char buf[80];
    if (s_active_valve_port > 0) {
        snprintf(buf, sizeof(buf), "Last: Moved to %d µL via Port %d", vol,
                 s_active_valve_port);
    } else {
        snprintf(buf, sizeof(buf), "Last: Moved to %d µL", vol);
    }
    move_history_set(buf);
}

static void create_move_tab(lv_obj_t *parent)
{
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(parent, 6, LV_PART_MAIN);

    s_move_target_label = lv_label_create(parent);
    /* Per-label font override — the default theme sets text_font on
     * labels directly, which prevents inheritance from the screen-level
     * style we set in ui_create. Apply explicitly so µ renders. */
    lv_obj_set_style_text_font(s_move_target_label, &s_font_main,
                               LV_PART_MAIN);
    lv_label_set_text(s_move_target_label, "Target: 0 µL");

    s_move_slider = lv_slider_create(parent);
    lv_slider_set_range(s_move_slider, 0, DEFAULT_SYRINGE_UL);
    lv_slider_set_value(s_move_slider, 0, LV_ANIM_OFF);
    /* 92% width + 10 px left margin keeps the slider knob fully on-screen
     * at value 0 (the knob extends past the bar by half its size). */
    lv_obj_set_width(s_move_slider, LV_PCT(92));
    lv_obj_set_style_margin_left(s_move_slider, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(s_move_slider, move_slider_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_move_valve_label = lv_label_create(parent);
    lv_label_set_text(s_move_valve_label, "Connected: Port --");

    /* Single centered "C Actuation" button — positioned where the
     * Aspirate/Dispense pair used to be, sized to span their combined
     * width so the button is the visual center of the tab. */
    lv_obj_t *btn_row = lv_obj_create(parent);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_size(btn_row, LV_PCT(100), 56);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_move_actuate_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_move_actuate_btn, 200, 48);
    lv_obj_t *al = lv_label_create(s_move_actuate_btn);
    lv_label_set_text(al, "C Actuation");
    lv_obj_center(al);
    lv_obj_add_event_cb(s_move_actuate_btn, actuate_btn_event_cb,
                        LV_EVENT_CLICKED, NULL);

    /* History line at the bottom — updated on every enqueue. */
    s_move_history_label = lv_label_create(parent);
    /* Same per-label font override as the Target label — µ otherwise renders
     * as a missing glyph because the theme's default Montserrat-14 has no
     * U+00B5. */
    lv_obj_set_style_text_font(s_move_history_label, &s_font_main,
                               LV_PART_MAIN);
    lv_label_set_text(s_move_history_label, "Last: --");
    lv_obj_set_style_text_color(s_move_history_label,
                                lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
}

/* ----------------------------------------------------------------- Prime
 *
 * LVGL 9.x dropped the all-in-one ``lv_msgbox_create(parent, title,
 * body, btns, close)`` signature and ``lv_msgbox_get_active_btn_text``;
 * we now build the modal piecewise and attach a click handler to each
 * footer button. The handler identifies which button was pressed by
 * reading its child label. ``user_data`` carries the modal pointer so
 * we can close it from the handler.
 */
/* Option list for both source and sink dropdowns — the four valve
 * positions, in order, so dropdown index i ↔ valve position i+1
 * (matching ``VALVE_POSITION_LABELS``). */
static const char PRIME_PORT_OPTIONS[] =
    "Port 1 to Path 1\nPort 3 to Path 1\nPort 1 to Path 2\nPort 3 to Path 2";

static int dropdown_index_to_position(uint16_t idx)
{
    return (int)idx + 1;
}

static uint16_t position_to_dropdown_index(int position)
{
    return (uint16_t)(position - 1);
}

static void prime_button_label_refresh(void)
{
    if (s_prime_btn_label == NULL) {
        return;
    }
    lv_label_set_text_fmt(s_prime_btn_label, "START\n%d cycle%s",
                          s_prime_cycles, (s_prime_cycles == 1) ? "" : "s");
}

static int prime_volume_uL(void)
{
    if (s_prime_volume_slider == NULL) {
        return 0;
    }
    return (int)lv_slider_get_value(s_prime_volume_slider);
}

static void prime_volume_label_refresh(void)
{
    if (s_prime_volume_label != NULL) {
        lv_label_set_text_fmt(s_prime_volume_label, "%d µL", prime_volume_uL());
    }
}

static void prime_volume_changed_cb(lv_event_t *e)
{
    (void)e;
    prime_volume_label_refresh();
}

static void prime_cycles_refresh(void)
{
    if (s_prime_cycles_label != NULL) {
        lv_label_set_text_fmt(s_prime_cycles_label, "%d", s_prime_cycles);
    }
    prime_button_label_refresh();
}

/* Wrap-around: at 1 the - button jumps to 20, at 20 the + button jumps
 * to 1. Matches the operator's expectation that the counter is a small
 * cyclic range rather than a clamp. */
static void prime_cycles_dec_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_prime_cycles = (s_prime_cycles > 1) ? s_prime_cycles - 1 : 20;
    prime_cycles_refresh();
}

static void prime_cycles_inc_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_prime_cycles = (s_prime_cycles < 20) ? s_prime_cycles + 1 : 1;
    prime_cycles_refresh();
}

/* Source and sink are now chosen independently from all four valve
 * positions (no auto-pairing). The same-position case is rejected at
 * START time in ``prime_btn_event_cb`` rather than by forcing a flip. */
static void prime_source_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    s_prime_source = dropdown_index_to_position(lv_dropdown_get_selected(
        (lv_obj_t *)lv_event_get_target(e)));
    prime_button_label_refresh();
}

static void prime_sink_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    s_prime_sink = dropdown_index_to_position(lv_dropdown_get_selected(
        (lv_obj_t *)lv_event_get_target(e)));
    prime_button_label_refresh();
}

static void prime_button_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *modal = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *txt = label ? lv_label_get_text(label) : NULL;
    if (txt != NULL && strcmp(txt, "Start") == 0) {
        pump_cmd_t cmd = {
            .kind = PUMP_CMD_PRIME,
            .payload.prime = {.cycles = s_prime_cycles,
                              .source_port = s_prime_source,
                              .sink_port = s_prime_sink,
                              .volume_uL = (float)prime_volume_uL()},
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
    /* Same valve position for source and sink would aspirate and
     * dispense through one physical port — fluid never reaches a
     * different port. Reject it (this is the exact failure the 4-way
     * selector fixes). */
    if (s_prime_source == s_prime_sink) {
        ui_show_toast("Source and sink must be different positions");
        return;
    }
    lv_obj_t *modal = lv_msgbox_create(NULL);
    lv_obj_set_style_text_font(modal, &s_font_main, LV_PART_MAIN);
    lv_msgbox_add_title(modal, "Aspirate / Dispense");
    char body[200];
    snprintf(body, sizeof(body),
             "Run %d cycle%s:\n"
             "%s (source)\n→ %s (sink)\n"
             "Each cycle = aspirate %d µL, then dispense to 0.",
             s_prime_cycles, (s_prime_cycles == 1) ? "" : "s",
             VALVE_POSITION_LABELS[s_prime_source - 1],
             VALVE_POSITION_LABELS[s_prime_sink - 1], prime_volume_uL());
    lv_msgbox_add_text(modal, body);
    lv_obj_t *start = lv_msgbox_add_footer_button(modal, "Start");
    lv_obj_t *cancel = lv_msgbox_add_footer_button(modal, "Cancel");
    lv_obj_add_event_cb(start, prime_button_cb, LV_EVENT_CLICKED, modal);
    lv_obj_add_event_cb(cancel, prime_button_cb, LV_EVENT_CLICKED, modal);
    lv_obj_center(modal);
}

static void create_prime_tab(lv_obj_t *parent)
{
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(parent, 4, LV_PART_MAIN);
    /* Four input groups + action button is a tall stack; allow vertical
     * scroll only (never horizontal) so the extra Target row can never
     * clip the button off the bottom edge. */
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);

    /* Row 1: Cycles [ - ] N [ + ] */
    lv_obj_t *row_cycles = lv_obj_create(parent);
    lv_obj_set_size(row_cycles, LV_PCT(100), 36);
    lv_obj_set_style_pad_all(row_cycles, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_cycles, 0, LV_PART_MAIN);
    lv_obj_set_layout(row_cycles, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_cycles, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_cycles, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_cycles, 6, LV_PART_MAIN);

    lv_obj_t *cycles_caption = lv_label_create(row_cycles);
    lv_label_set_text(cycles_caption, "Cycle (1~20):");
    lv_obj_set_width(cycles_caption, 110);

    lv_obj_t *dec_btn = lv_btn_create(row_cycles);
    lv_obj_set_size(dec_btn, 36, 32);
    lv_obj_t *dec_lbl = lv_label_create(dec_btn);
    lv_label_set_text(dec_lbl, "-");
    lv_obj_center(dec_lbl);
    lv_obj_add_event_cb(dec_btn, prime_cycles_dec_cb, LV_EVENT_CLICKED, NULL);

    s_prime_cycles_label = lv_label_create(row_cycles);
    lv_obj_set_width(s_prime_cycles_label, 32);
    lv_obj_set_style_text_align(s_prime_cycles_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_text(s_prime_cycles_label, "1");

    lv_obj_t *inc_btn = lv_btn_create(row_cycles);
    lv_obj_set_size(inc_btn, 36, 32);
    lv_obj_t *inc_lbl = lv_label_create(inc_btn);
    lv_label_set_text(inc_lbl, "+");
    lv_obj_center(inc_lbl);
    lv_obj_add_event_cb(inc_btn, prime_cycles_inc_cb, LV_EVENT_CLICKED, NULL);

    /* Row 2: Source dropdown */
    lv_obj_t *row_src = lv_obj_create(parent);
    lv_obj_set_size(row_src, LV_PCT(100), 36);
    lv_obj_set_style_pad_all(row_src, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_src, 0, LV_PART_MAIN);
    lv_obj_set_layout(row_src, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_src, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_src, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_src, 6, LV_PART_MAIN);

    lv_obj_t *src_caption = lv_label_create(row_src);
    lv_label_set_text(src_caption, "Source:");
    lv_obj_set_width(src_caption, 64);

    s_prime_source_dd = lv_dropdown_create(row_src);
    lv_dropdown_set_options(s_prime_source_dd, PRIME_PORT_OPTIONS);
    lv_dropdown_set_selected(s_prime_source_dd,
                             position_to_dropdown_index(s_prime_source));
    /* LV_SYMBOL_DOWN is a FontAwesome glyph (U+F078) carried by LVGL's
     * built-in lv_font_montserrat_14. Our custom subset doesn't include
     * it, but the font-fallback chain set in ``ui_create`` redirects
     * missing glyphs to that built-in font, so the caret renders. */
    lv_dropdown_set_symbol(s_prime_source_dd, LV_SYMBOL_DOWN);
    lv_obj_set_flex_grow(s_prime_source_dd, 1); /* fill row — labels are wide */
    lv_obj_add_event_cb(s_prime_source_dd, prime_source_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* Row 3: Sink dropdown */
    lv_obj_t *row_sink = lv_obj_create(parent);
    lv_obj_set_size(row_sink, LV_PCT(100), 36);
    lv_obj_set_style_pad_all(row_sink, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_sink, 0, LV_PART_MAIN);
    lv_obj_set_layout(row_sink, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_sink, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_sink, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_sink, 6, LV_PART_MAIN);

    lv_obj_t *sink_caption = lv_label_create(row_sink);
    lv_label_set_text(sink_caption, "Sink:");
    lv_obj_set_width(sink_caption, 64);

    s_prime_sink_dd = lv_dropdown_create(row_sink);
    lv_dropdown_set_options(s_prime_sink_dd, PRIME_PORT_OPTIONS);
    lv_dropdown_set_selected(s_prime_sink_dd,
                             position_to_dropdown_index(s_prime_sink));
    lv_dropdown_set_symbol(s_prime_sink_dd, LV_SYMBOL_DOWN);
    lv_obj_set_flex_grow(s_prime_sink_dd, 1); /* fill row — labels are wide */
    lv_obj_add_event_cb(s_prime_sink_dd, prime_sink_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* Row 4: Target µL slider (per-cycle aspirate volume), mirroring the
     * Move tab's Target bar: [ Target: ][ ====slider==== ][ N µL ]. */
    lv_obj_t *row_vol = lv_obj_create(parent);
    lv_obj_set_size(row_vol, LV_PCT(100), 36);
    lv_obj_set_style_pad_all(row_vol, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_vol, 0, LV_PART_MAIN);
    lv_obj_set_layout(row_vol, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_vol, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_vol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_vol, 6, LV_PART_MAIN);

    lv_obj_t *vol_caption = lv_label_create(row_vol);
    lv_label_set_text(vol_caption, "Target:");
    lv_obj_set_width(vol_caption, 64);

    s_prime_volume_slider = lv_slider_create(row_vol);
    lv_slider_set_range(s_prime_volume_slider, 0, DEFAULT_SYRINGE_UL);
    lv_slider_set_value(s_prime_volume_slider, DEFAULT_SYRINGE_UL, LV_ANIM_OFF);
    lv_obj_set_flex_grow(s_prime_volume_slider, 1);
    lv_obj_add_event_cb(s_prime_volume_slider, prime_volume_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_prime_volume_label = lv_label_create(row_vol);
    /* Per-label font override so µ renders (theme default lacks U+00B5). */
    lv_obj_set_style_text_font(s_prime_volume_label, &s_font_main,
                               LV_PART_MAIN);
    lv_obj_set_width(s_prime_volume_label, 58);
    lv_obj_set_style_text_align(s_prime_volume_label, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    prime_volume_label_refresh();

    /* Row 5: START button with dynamic label. */
    s_prime_btn = lv_btn_create(parent);
    lv_obj_set_size(s_prime_btn, LV_PCT(100), 64);
    s_prime_btn_label = lv_label_create(s_prime_btn);
    lv_obj_set_style_text_font(s_prime_btn_label, &s_font_main,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(s_prime_btn_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_center(s_prime_btn_label);
    lv_obj_add_event_cb(s_prime_btn, prime_btn_event_cb, LV_EVENT_CLICKED,
                        NULL);
    prime_button_label_refresh();

    /* Spinner overlay during BUSY. */
    s_prime_spinner = lv_spinner_create(parent);
    lv_spinner_set_anim_params(s_prime_spinner, 1000, 60);
    lv_obj_set_size(s_prime_spinner, 60, 60);
    lv_obj_add_flag(s_prime_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_prime_spinner, LV_ALIGN_CENTER, 0, 0);

    s_prime_label = lv_label_create(parent);
    lv_label_set_text(s_prime_label, "");
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
static void reconnect_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    pump_cmd_t cmd = {.kind = PUMP_CMD_DIAGNOSE};
    enqueue_or_toast(&cmd, "Pump busy");
}

static void create_status_tab(lv_obj_t *parent)
{
    /* Stack the status table over a Reconnect button in a flex column.
     * Horizontal scroll on the tab is disabled — there's nothing to
     * scroll left/right on a 2-column status table that already fits. */
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(parent, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(parent, LV_DIR_NONE);

    s_status_table = lv_table_create(parent);
    lv_obj_set_width(s_status_table, LV_PCT(100));
    lv_obj_set_flex_grow(s_status_table, 1); /* fill remaining height */
    /* Allow vertical scrolling only (table can grow past viewport),
     * never horizontal. */
    lv_obj_set_scroll_dir(s_status_table, LV_DIR_VER);
    lv_table_set_row_cnt(s_status_table, STATUS_ROW_COUNT);
    lv_table_set_col_cnt(s_status_table, 2);
    lv_table_set_col_width(s_status_table, 0, 130);
    lv_table_set_col_width(s_status_table, 1, 175);
    for (int i = 0; i < STATUS_ROW_COUNT; ++i) {
        lv_table_set_cell_value(s_status_table, (uint16_t)i, 0,
                                STATUS_ROW_NAMES[i]);
        lv_table_set_cell_value(s_status_table, (uint16_t)i, 1, "--");
    }

    s_reconnect_btn = lv_btn_create(parent);
    lv_obj_set_size(s_reconnect_btn, LV_PCT(100), 32);
    lv_obj_t *rl = lv_label_create(s_reconnect_btn);
    lv_label_set_text(rl, "Reconnect");
    lv_obj_center(rl);
    lv_obj_add_event_cb(s_reconnect_btn, reconnect_btn_event_cb,
                        LV_EVENT_CLICKED, NULL);
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
    set_disabled(s_move_actuate_btn, !enabled);
    set_disabled(s_prime_btn, !enabled);
    set_disabled(s_prime_volume_slider, !enabled);
}

/* ----------------------------------------------------------------- ui_create
 */
void ui_create(void)
{
    /* Build the runtime font wrapper: copy the const generated struct
     * into the mutable ``s_font_main`` and graft LVGL's built-in
     * Montserrat-14 (which carries FontAwesome icons) on as the
     * fallback chain. Every subsequent reference uses ``&s_font_main``
     * — see the file-scope comment for the rationale. */
    s_font_main = montserrat_14_ext;
    s_font_main.fallback = &lv_font_montserrat_14;

    /* Reinitialise the LVGL default theme so the Unicode-rich Montserrat
     * font is applied to every widget — labels inside buttons, tables,
     * msgboxes, toasts, all of them. The BSP set up the theme during
     * bsp_display_start with the built-in Montserrat-14, which has no
     * µ / em-dash / → glyphs. Replacing the theme once here makes the
     * per-label font set calls below redundant; we keep them as
     * belt-and-suspenders. */
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL) {
        lv_theme_t *theme = lv_theme_default_init(
            disp, lv_palette_main(LV_PALETTE_BLUE),
            lv_palette_main(LV_PALETTE_RED), false, &s_font_main);
        lv_display_set_theme(disp, theme);
    }
    lv_obj_set_style_text_font(lv_scr_act(), &s_font_main, LV_PART_MAIN);

    s_banner = lv_label_create(lv_scr_act());
    /* Banner shows error banners that contain — (em-dash) and → from
     * main.c (e.g. "diagnose failed — check server", "Needs Initialize
     * (press →)"); explicit font set for the same reason as move labels. */
    lv_obj_set_style_text_font(s_banner, &s_font_main, LV_PART_MAIN);
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

    /* Disable horizontal swipe on the tab content so tab switching only
     * happens via the top tab bar (touch-on-tab-name). */
    lv_obj_remove_flag(lv_tabview_get_content(s_tabview),
                       LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *valve_tab = lv_tabview_add_tab(s_tabview, "Valve");
    lv_obj_t *move_tab = lv_tabview_add_tab(s_tabview, "Move");
    lv_obj_t *prime_tab = lv_tabview_add_tab(s_tabview, "Asp/Disp");
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
    /* Re-range the volume slider against the server-reported syringe
     * size. syringe_uL == 0 means diagnose hasn't landed yet; leave the
     * default range in that case. We also re-set the value so it
     * doesn't fall off the end of a now-smaller range. */
    if (s_move_slider != NULL && status->syringe_uL > 0.0f) {
        int max_uL = (int)status->syringe_uL;
        int cur = (int)lv_slider_get_value(s_move_slider);
        if (cur > max_uL) {
            cur = max_uL;
        }
        lv_slider_set_range(s_move_slider, 0, max_uL);
        lv_slider_set_value(s_move_slider, cur, LV_ANIM_OFF);
        if (s_move_target_label != NULL) {
            lv_label_set_text_fmt(s_move_target_label, "Target: %d µL", cur);
        }
    }
    /* Same re-range for the Aspirate/Dispense per-cycle volume slider. */
    if (s_prime_volume_slider != NULL && status->syringe_uL > 0.0f) {
        int max_uL = (int)status->syringe_uL;
        int cur = (int)lv_slider_get_value(s_prime_volume_slider);
        if (cur > max_uL) {
            cur = max_uL;
        }
        lv_slider_set_range(s_prime_volume_slider, 0, max_uL);
        lv_slider_set_value(s_prime_volume_slider, cur, LV_ANIM_OFF);
        prime_volume_label_refresh();
    }
    /* Valve highlight from the cached server-reported port. */
    if (status->valve[0] >= '1' && status->valve[0] <= '4' &&
        status->valve[1] == '\0') {
        valve_highlight_port(status->valve[0] - '0');
    } else {
        valve_highlight_port(-1);
    }
    /* Move tab — "Connected: Port N to Path M" mirrors the Valve tab
     * button labels so the operator sees the same wording in both
     * tabs. Mapping per ``VALVE_POSITION_LABELS`` in create_valve_tab. */
    if (s_move_valve_label != NULL) {
        static const char *VALVE_CONNECTED_LABELS[4] = {
            "Connected: Port 1 to Path 1", /* valve = "1" */
            "Connected: Port 3 to Path 1", /* valve = "2" */
            "Connected: Port 1 to Path 2", /* valve = "3" */
            "Connected: Port 3 to Path 2", /* valve = "4" */
        };
        if (status->valve[0] >= '1' && status->valve[0] <= '4' &&
            status->valve[1] == '\0') {
            int idx = status->valve[0] - '1';
            lv_label_set_text(s_move_valve_label,
                              VALVE_CONNECTED_LABELS[idx]);
        } else {
            lv_label_set_text(s_move_valve_label, "Connected: --");
        }
    }
}

/* ----------------------------------------------------------------- Error modal
 *
 * Same LVGL 9.x adaptation as the Prime modal: build piecewise, attach
 * a click handler per footer button, identify the button by its child
 * label text.
 */
static void modal_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *modal = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *txt = label ? lv_label_get_text(label) : NULL;
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
    /* "Dismiss" and any other label: just close. */
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

    char body[280];
    snprintf(body, sizeof(body), "%s (code %d, HTTP %d)\n%s\n%s%s%s",
             err->error_name, err->code, err->http_status, err->message,
             err->command[0] != '\0' ? "cmd " : "",
             err->command[0] != '\0' ? err->command : "",
             err->command[0] != '\0' ? "\n" : "");

    s_modal = lv_msgbox_create(NULL);
    /* err->message can contain — (em-dash) — set font on the modal so
     * the body text label inherits properly. */
    lv_obj_set_style_text_font(s_modal, &s_font_main, LV_PART_MAIN);
    lv_msgbox_add_title(s_modal, err->klass == PUMP_ERROR_FATAL
                                     ? "Fatal pump error"
                                     : "Pump error");
    lv_msgbox_add_text(s_modal, body);

    if (err->klass == PUMP_ERROR_FATAL) {
        lv_obj_t *reinit =
            lv_msgbox_add_footer_button(s_modal, "Re-initialize");
        lv_obj_add_event_cb(reinit, modal_event_cb, LV_EVENT_CLICKED, s_modal);
    } else {
        lv_obj_t *retry = lv_msgbox_add_footer_button(s_modal, "Retry");
        lv_obj_t *dismiss = lv_msgbox_add_footer_button(s_modal, "Dismiss");
        lv_obj_add_event_cb(retry, modal_event_cb, LV_EVENT_CLICKED, s_modal);
        lv_obj_add_event_cb(dismiss, modal_event_cb, LV_EVENT_CLICKED, s_modal);
    }
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
