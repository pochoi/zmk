/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
#include <zephyr/random/rand32.h>
#include <zmk/display.h>
#include "peripheral_status.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zephyr/random/rand32.h>

#define LEN_FRAMES 94310
#define LEN_DICT 2048
#define FPS 15
#define NUM_SAND 1654
#define BOARD_R 68
#define BOARD_C 136
#define SAND_W 48
#define SAND_H 100
#define C_OFFSET 18
#define R_OFFSET 10
#define FRAME_L (BOARD_R * BOARD_C / 8 + 8)

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);
static const uint16_t ms_per_frame = 1000 / FPS;

typedef enum { BLANK = 0, WALL, SAND, BARRIER } Pixel;

struct peripheral_status_state {
    bool connected;
};

extern uint8_t board[BOARD_R][BOARD_C];
extern uint8_t board_bits[FRAME_L];
extern uint8_t sand_coords[NUM_SAND][2];
static uint32_t frame_counter;
static uint32_t lcg;
static lv_draw_img_dsc_t img_dsc;
static lv_obj_t *video;

K_WORK_DEFINE(anim_work, anim_work_handler);

void anim_expiry_function() { k_work_submit(&anim_work); }
#define FRAME_SIZE (ANIM_SIZE * ANIM_SIZE / 8 + 8)
K_TIMER_DEFINE(anim_timer, anim_expiry_function, NULL);
uint8_t new_frame[FRAME_SIZE] = {
    0x00, 0x00, 0x00, 0xff, /*Color of index 0*/
    0xff, 0xff, 0xff, 0xff  /*Color of index 1*/
};
static int8_t direction;
static int8_t same_frames;
lv_img_dsc_t frame = {.header.cf = LV_IMG_CF_INDEXED_1BIT,
                      .header.always_zero = 0,
                      .header.reserved = 0,
                      .header.w = BOARD_C,
                      .header.h = BOARD_R,
                      .data_size = FRAME_L,
                      .data = board_bits};
void anim_work_handler(struct k_work *work) {
    for (int s_left = NUM_SAND - 1; s_left > 0; s_left--) {
        // LCG to pick index of sand grain to shuffle
        lcg = lcg * 22695477 + 1;
        // Fisher-Yates shuffle the sand array
        uint16_t sand_idx = lcg % s_left;
        uint8_t *sand_selected = sand_coords[sand_idx];
        uint8_t *sand_current = sand_coords[s_left];
        // Swapped element is to be updated
        uint8_t row = sand_selected[1];
        uint8_t col = sand_selected[0];
        sand_selected[0] = sand_current[0];
        sand_selected[1] = sand_current[1];
        bool update = false;
        uint8_t nr, nc;
        if (!board[row][col + direction]) {
            nr = row;
            nc = col + direction;
            update = true;
        } else {
            bool can_go_left = !board[row - 1][col];
            bool can_go_right = !board[row + 1][col];
            nc = col;
            if (can_go_left & can_go_right) {
                if (lcg >> 16 & 0x1) {
                    nr = row - 1;
                } else {
                    nr = row + 1;
                }
                update = true;
            } else if (can_go_left) {
                nr = row - 1;
                update = true;
            } else if (can_go_right) {
                nr = row + 1;
                update = true;
            }
        }
        if (update) {
            // Updating the game board
            board[row][col] = BLANK;
            board[nr][nc] = SAND;
            // Calculating byte coordinates and value of original pixel
            uint16_t temp = row * BOARD_C + col;
            uint16_t byte_idx = (temp >> 3) + 8;
            uint8_t bit_idx = temp & 0x7;
            board_bits[byte_idx] ^= (0x80 >> bit_idx);
            // Calculating byte coordinates and value of new pixel
            temp = nr * BOARD_C + nc;
            byte_idx = (temp >> 3) + 8;
            bit_idx = temp & 0x7;
            board_bits[byte_idx] ^= (0x80 >> bit_idx);
            // Updating sand
            sand_current[0] = nc;
            sand_current[1] = nr;
        } else {
            sand_current[0] = col;
            sand_current[1] = row;
        }
    }
    lv_canvas_draw_img(video, 0, 0, &frame, &img_dsc);
    // Set timer to go off when animation finishes
    k_timer_start(&anim_timer, K_MSEC(ms_per_frame), K_MSEC(ms_per_frame));
    for (int r = 32; r < 36; r++) {
        if (board[r][67] == board[r][68]) {
            same_frames = 0;
            return;
        }
    }
    for (int r = 32; r < 35; r++) {
        if (board[r][67] != board[r + 1][67] || board[r][68] != board[r + 1][68]) {
            same_frames = 0;
            return;
        }
    }
    same_frames++;
    if (same_frames > 15) {
        same_frames = 0;
        direction *= -1;
    }
}

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], struct status_state state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, DISP_WIDTH, BATTERY_HEIGHT + 3, &rect_black_dsc);

    // Draw battery
    draw_battery(canvas, state);

    // Draw output status
    lv_canvas_draw_text(canvas, 0, 0, DISP_WIDTH, &label_dsc,
                        state.connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    return (struct battery_status_state) {
        .level = bt_bas_get_battery_level(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

static struct peripheral_status_state get_state(const zmk_event_t *_eh) {
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

static void set_connection_status(struct zmk_widget_status *widget,
                                  struct peripheral_status_state state) {
    widget->state.connected = state.connected;

    draw_top(widget->obj, widget->cbuf, widget->state);
}

static void output_status_update_cb(struct peripheral_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_connection_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_split_peripheral_status_changed);

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, BATTERY_OFFSET, 0);
    lv_canvas_set_buffer(top, widget->cbuf, DISP_WIDTH, BATTERY_HEIGHT + 3, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();

    video = lv_canvas_create(widget->obj);
    lv_obj_align(video, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(video, widget->vbuf, 136, DISP_WIDTH, LV_IMG_CF_TRUE_COLOR);
    lv_draw_img_dsc_init(&img_dsc);
    frame_counter = 0;
    lcg = sys_rand32_get();
    direction = 1;
    same_frames = 0;
    // Starting animation timer
    k_timer_start(&anim_timer, K_MSEC(10), K_MSEC(10));
    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }