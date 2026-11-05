/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Waveshare ESP32-S3-Knob-Touch-LCD-1.8: ESP32-S3R8 (8 MB octal PSRAM) with
 * 16 MB flash, a round 360 px ST77916 QSPI panel with CST816 touch, a rotary
 * encoder, a PDM mic and a PCM5100A DAC to a 3.5 mm jack (no speaker). A
 * second chip, an ESP32-U4WDH, sits on the other side of the USB-C plug
 * (through a CH34x bridge) and keeps its own firmware; this profile runs on
 * the S3 only.
 *
 * Sources: the Waveshare wiki (waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8),
 * its schematic (ESP32-S3-Knob-Touch-LCD-1.8-schematic.zip) and demos
 * (ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip: 08_LVGL_Test for the panel, touch
 * and backlight, 07_Audio_Test for I2S and PDM, 04_Encoder_Test for the knob).
 * The panel's init sequence is the 1.85C's, which the demo's matches command
 * for command.
 *
 * Input: BOOT is a recessed button that takes a paperclip, so talking is the
 * on-screen mic (touch_talk), and a tap on the screen confirms pairing. BOOT
 * still works as push-to-talk. The knob is a two-way switch, not a quadrature
 * encoder: each detent pulses one line low, A turning right and B turning
 * left (the demo's bidi_switch_knob.c). Each detent steps the home pages
 * (MUSE_BTN_PAGE_*): right toward Settings, left toward the pocket watch and,
 * past it, the app that runs. The volume is in Settings > Sound.
 *
 * Audio: a CH445P switch picks which chip drives the DAC, selected by the
 * GPIO0 net. With BOOT up (GPIO0 high) it's the S3, so the DAC only hears
 * the ESP32 while BOOT is held, which is push-to-talk. The DAC's soft mute
 * (XSMT) is on the ESP32's GPIO32, so the jack is only live while the
 * ESP32's firmware unmutes it, as Waveshare's does.
 *
 * Checked on one unit: display, touch, the mic's self-test, the knob's steps
 * and BLE advertising. Not yet checked: the jack (no headphones to hand).
 */
#include <math.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_ui.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "waveshare_s3_185c/st77916_init_185c.h"

static const char *TAG = "board";

/* Pins (schematic sheet 2, ESP32S3-R8). */
#define LCD_SCLK GPIO_NUM_13
#define LCD_CS GPIO_NUM_14
#define LCD_D0 GPIO_NUM_15
#define LCD_D1 GPIO_NUM_16
#define LCD_D2 GPIO_NUM_17
#define LCD_D3 GPIO_NUM_18
#define LCD_RST GPIO_NUM_21
#define LCD_BL GPIO_NUM_47     /* through a MOSFET, active high */
#define LCD_RES 360

#define I2C_SDA GPIO_NUM_11    /* touch, and the DRV2605 haptic driver (unused) */
#define I2C_SCL GPIO_NUM_12
#define TOUCH_INT GPIO_NUM_9
#define TOUCH_RST GPIO_NUM_10

#define KNOB_A GPIO_NUM_8      /* EC1_A, external pull-up */
#define KNOB_B GPIO_NUM_7      /* EC1_B, external pull-up */

#define I2S_BCLK GPIO_NUM_39   /* to the PCM5100A, through the CH445P */
#define I2S_WS GPIO_NUM_40
#define I2S_DOUT GPIO_NUM_41
#define MIC_CLK GPIO_NUM_45    /* MSM261 PDM mic, L/R to GND */
#define MIC_DATA GPIO_NUM_46

#define TALK_GPIO GPIO_NUM_0   /* BOOT */

#define DRAW_BUF_LINES 72      /* five bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define MIC_GAIN_OFFSET_DB 0

#define KNOB_LOCKOUT 3         /* polls (30 ms) a line's later edges are its bounce */

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_touch_indev;
static muse_gpio_button_t s_boot;
static pcnt_unit_handle_t s_knob_right, s_knob_left;
static i2s_chan_handle_t s_tx, s_mic;
static int32_t s_mic_dc;
static bool s_mic_settled;
static int s_mic_gain_q8 = 256;

/* The ST77916 needs even-aligned update windows over the QSPI command path. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/* One counter per line, counting its falling edges: a pulse can be shorter
 * than a 10 ms poll, so the hardware catches it and poll_knob() reads it. */
static esp_err_t knob_line_init(gpio_num_t gpio, pcnt_unit_handle_t *unit)
{
    const pcnt_unit_config_t unit_cfg = { .low_limit = -100, .high_limit = 100 };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, unit), TAG, "pcnt");
    const pcnt_glitch_filter_config_t filter = { .max_glitch_ns = 10000 };
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(*unit, &filter), TAG, "pcnt filter");
    pcnt_channel_handle_t ch;
    const pcnt_chan_config_t ch_cfg = { .edge_gpio_num = gpio, .level_gpio_num = -1 };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(*unit, &ch_cfg, &ch), TAG, "pcnt channel");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_HOLD,
                                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE),
                        TAG, "pcnt edge");
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(*unit), TAG, "pcnt enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(*unit), TAG, "pcnt clear");
    return pcnt_unit_start(*unit);
}

static esp_err_t knob_init(void)
{
    ESP_RETURN_ON_ERROR(knob_line_init(KNOB_A, &s_knob_right), TAG, "knob a");
    return knob_line_init(KNOB_B, &s_knob_left);
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, TALK_GPIO), TAG, "boot button");
    /* BOOT is held at flashing time; don't count a held button as a press. */
    s_boot.pressed = gpio_get_level(TALK_GPIO) == 0;

    return knob_init();
}

/* muse_lcd_bands_register takes over the panel IO's color-done callback and,
 * with the bands, needs the bus set up on the same core as the UI task. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        ESP_LOGE(TAG, "backlight pwm");
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus");
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel io");
        return NULL;
    }
    st77916_vendor_config_t vendor_cfg = {
        .init_cmds = ST77916_INIT_185C_CMDS,
        .init_cmds_size = ST77916_INIT_185C_SIZE,
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "st77916");
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    esp_lcd_panel_io_handle_t tp_io;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch io");
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = TOUCH_RST,
        .int_gpio_num = TOUCH_INT,
    };
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        ESP_LOGE(TAG, "cst816");
        return NULL;
    }
    const esp_lv_adapter_touch_config_t tp_lvgl_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    *touch = esp_lv_adapter_register_touch(&tp_lvgl_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    s_touch_indev = *touch;
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
    vTaskDelay(pdMS_TO_TICKS(120));             /* settle before the next command */
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/* No codec: the PCM5100A takes I2S as is (FMT low, SCK grounded, so it clocks
 * from BCK) and esp_codec_dev adds the volume in software. The PDM mic is on
 * I2S0, the only S3 port that does PDM, and the DAC on I2S1. Both run all the
 * time; idle, the DAC gets zeros (auto_clear). */
static int data_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    (void)on;
    return ESP_CODEC_DEV_OK;
}

static int spk_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t wrote;
    return i2s_channel_write(s_tx, data, size, &wrote, portMAX_DELAY) == ESP_OK ? ESP_CODEC_DEV_OK
                                                                                : ESP_CODEC_DEV_WRITE_FAIL;
}

/* muse_audio reads two slots: the mono PDM samples are read into the front
 * half, cleared of the PDM filter's DC offset and the gain applied, then
 * spread to both slots from the back so nothing is overwritten unread. */
static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    int16_t *s = (int16_t *)data;
    const size_t frames = size / (2 * sizeof(int16_t));
    size_t got;
    if (i2s_channel_read(s_mic, s, frames * sizeof(int16_t), &got, pdMS_TO_TICKS(1000)) != ESP_OK ||
        got != frames * sizeof(int16_t)) {
        return ESP_CODEC_DEV_READ_FAIL;
    }
    if (!s_mic_settled) {
        int32_t sum = 0;
        for (size_t i = 0; i < frames; i++) {
            sum += s[i];
        }
        s_mic_dc = sum / (int32_t)frames * 256;
        s_mic_settled = true;
    }
    for (size_t i = frames; i-- > 0;) {
        s_mic_dc += (s[i] * 256 - s_mic_dc) >> 8;
        int v = (s[i] - (s_mic_dc >> 8)) * s_mic_gain_q8 >> 8;
        int16_t c = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
        s[2 * i] = c;
        s[2 * i + 1] = c;
    }
    return ESP_CODEC_DEV_OK;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    (void)mic;
    s_mic_gain_q8 = (int)(256.0f * powf(10.0f, (db - MIC_GAIN_OFFSET_DB) / 20.0f));
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_cfg, &s_tx, NULL), TAG, "i2s tx channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx on");

    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_cfg, NULL, &s_mic), TAG, "pdm channel");
    const i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_CLK,
            .din = MIC_DATA,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_mic, &pdm_cfg), TAG, "pdm rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_mic), TAG, "pdm on");

    static const audio_codec_data_if_t spk_if = { .enable = data_enable, .write = spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = data_enable, .read = mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    ESP_LOGI(TAG, "audio: PCM5100A on I2S1, PDM mic on I2S0, %d Hz", MUSE_AUDIO_RATE);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* Edges a line has counted since the last poll, past its bounce: a detent
 * reads as one step however many edges its contacts make. */
static bool knob_detent(pcnt_unit_handle_t unit, int *lockout)
{
    int n = 0;
    if (pcnt_unit_get_count(unit, &n) == ESP_OK && n) {
        pcnt_unit_clear_count(unit);
    }
    if (*lockout) {
        (*lockout)--;
        return false;
    }
    if (n) {
        *lockout = KNOB_LOCKOUT;
    }
    return n != 0;
}

/* A right turn is a page right, a left turn a page left; muse_input wakes a
 * dark screen with either and takes the step only when it's lit. */
static unsigned poll_knob(void)
{
    static int right_lockout, left_lockout;
    int step = knob_detent(s_knob_right, &right_lockout) - knob_detent(s_knob_left, &left_lockout);
    return step > 0 ? MUSE_BTN_PAGE_RIGHT : step < 0 ? MUSE_BTN_PAGE_LEFT : 0;
}

/* Talk comes from the on-screen mic (touch_talk) or BOOT. */
static unsigned poll_buttons(void)
{
    return poll_knob() | muse_gpio_button_poll(&s_boot);
}

/* The battery divider (GPIO1) is on the 5 V rail, so on USB it reads the
 * rail, not a cell: the battery reads as absent. */
static esp_err_t read_power(muse_power_t *out)
{
    *out = (muse_power_t){
        .usb = usb_serial_jtag_is_connected(),
        .battery_pct = -1,
    };
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_disp_sleep(s_panel, true);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* No PMU; deep sleep is the closest thing to off. BOOT wakes it. */
    esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Knob-Touch-LCD-1.8",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .touch_talk = true,
    .page_knob = true,
    .diagonal_in = 1.8f,
    .talk_button = "BOOT",
    /* The on-screen mic, low and central where the circle has room for it. */
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -28 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
