#include "ui/screens/screen_web.h"
#include "app/app_controller.h"
#include "touch/touch_driver.h"
#include "config/config_nmea_tester.h"
#include "system/port_activity.h"
#include "ui/activity_pulse.h"
#include "lvgl.h"

static lv_obj_t *s_screen;
static lv_obj_t *s_leds[PORT_ACTIVITY_COUNT];
static activity_pulse_t s_pulses[PORT_ACTIVITY_COUNT];
static lv_timer_t *s_activity_timer;

static void release_(lv_event_t *e) { (void)e; app_controller_return_local(); }
static void input_reset_(void)
{
    lv_indev_t *indev = touch_driver_get_indev();
    if (indev) { lv_indev_reset(indev, NULL); lv_indev_wait_release(indev); }
}

static lv_obj_t *stroke_(lv_obj_t *parent, int width, int height, int x, int y, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x45D6BC), 0);
    lv_obj_set_style_border_width(obj, 3, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *label_(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xC8D8E5), 0);
    lv_label_set_text(label, text);
    return label;
}

static void led_(lv_obj_t *parent, port_activity_channel_t channel, uint32_t color,
                 const char *name, int x)
{
    lv_obj_t *led = lv_led_create(parent);
    lv_obj_remove_style_all(led);
    lv_obj_set_size(led, 28, 28);
    lv_obj_set_pos(led, x, 44);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(led, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(led, lv_color_white(), 0);
    lv_obj_set_style_border_width(led, 1, 0);
    lv_obj_set_style_shadow_color(led, lv_color_white(), 0);
    lv_obj_set_style_shadow_width(led, 10, 0);
    lv_obj_set_style_shadow_opa(led, LV_OPA_50, 0);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_led_set_color(led, lv_color_hex(color));
    lv_led_off(led);
    s_leds[channel] = led;
    lv_obj_t *label = label_(parent, name, &lv_font_montserrat_18);
    lv_obj_set_pos(label, x + 37, 47);
}

static void port_pair_(const char *name, int x, int y,
                       port_activity_channel_t tx, uint32_t tx_color,
                       port_activity_channel_t rx, uint32_t rx_color)
{
    lv_obj_t *pair = lv_obj_create(s_screen);
    lv_obj_remove_style_all(pair);
    lv_obj_set_size(pair, 200, 90);
    lv_obj_set_pos(pair, x, y);
    lv_obj_clear_flag(pair, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = label_(pair, name, &lv_font_montserrat_20);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
    led_(pair, tx, tx_color, "TX", 16);
    led_(pair, rx, rx_color, "RX", 114);
}

static void activity_reset_(void)
{
    (void)port_activity_take();
    uint32_t now = lv_tick_get();
    for (unsigned i = 0; i < PORT_ACTIVITY_COUNT; ++i) {
        activity_pulse_reset(&s_pulses[i], now);
        lv_led_off(s_leds[i]);
    }
}

static void activity_timer_(lv_timer_t *timer)
{
    (void)timer;
    if (lv_scr_act() != s_screen) return;
    uint32_t events = port_activity_take(), now = lv_tick_get();
    for (unsigned i = 0; i < PORT_ACTIVITY_COUNT; ++i) {
        bool was_lit = s_pulses[i].lit;
        bool lit = activity_pulse_update(&s_pulses[i], (events & (1u << i)) != 0, now);
        if (lit != was_lit) {
            if (lit) lv_led_on(s_leds[i]);
            else lv_led_off(s_leds[i]);
        }
    }
}

void screen_web_show(void)
{
    input_reset_();
    if (!s_screen) {
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x101D2C), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
        bool compact = LCD_HEIGHT < 400;
        int size = compact ? 64 : 96;
        int top = (LCD_HEIGHT - (compact ? 278 : 350)) / 2;
        int x = (LCD_WIDTH - size) / 2;
        stroke_(s_screen, size, size, x, top, LV_RADIUS_CIRCLE);
        stroke_(s_screen, size * 45 / 100, size, x + size * 28 / 100, top, LV_RADIUS_CIRCLE);
        stroke_(s_screen, size - 2, 3, x + 1, top + size / 2, 0);
        lv_obj_t *title = label_(s_screen, "WEB CONTROL", &lv_font_montserrat_22);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, top + size + 14);
        int pairs_x = (LCD_WIDTH - 440) / 2, pairs_y = top + (compact ? 116 : 154);
        port_pair_("RS-485", pairs_x, pairs_y,
                   PORT_ACTIVITY_RS485_TX, 0xFFD247, PORT_ACTIVITY_RS485_RX, 0x50E384);
        port_pair_("CAN", pairs_x + 240, pairs_y,
                   PORT_ACTIVITY_CAN_TX, 0x459CFF, PORT_ACTIVITY_CAN_RX, 0xF4F8FF);
        lv_obj_t *button = lv_btn_create(s_screen);
        lv_obj_set_size(button, 300, compact ? 56 : 70);
        lv_obj_align(button, LV_ALIGN_TOP_MID, 0, top + (compact ? 222 : 280));
        lv_obj_set_style_bg_color(button, lv_color_hex(0x247A6E), 0);
        lv_obj_add_event_cb(button, release_, LV_EVENT_CLICKED, NULL);
        lv_obj_t *label = label_(button, "Return control", &lv_font_montserrat_22);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
        s_activity_timer = lv_timer_create(activity_timer_, 40, NULL);
    }
    activity_reset_();
    lv_scr_load(s_screen);
    if (s_activity_timer) {
        lv_timer_reset(s_activity_timer);
        lv_timer_resume(s_activity_timer);
    }
}

void screen_web_hide(void)
{
    input_reset_();
    if (s_activity_timer) lv_timer_pause(s_activity_timer);
    if (s_screen) activity_reset_();
}
