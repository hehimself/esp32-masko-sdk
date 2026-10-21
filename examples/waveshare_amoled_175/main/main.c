/**
 * @file main.c
 * @brief Play a `.mkpk` pack on the Waveshare ESP32-S3-Touch-AMOLED-1.75.
 *
 * Panel init comes from the Waveshare BSP. Everything after that is masko_*.
 *
 * The pack lives in the `masko` flash partition and is mmapped, so it is not
 * compiled into the app binary. Flash the partition once with
 * `tools/flash_pack.sh`.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"

#include "masko_pack.h"
#include "masko_calib.h"
#include "masko_display.h"
#include "masko_player.h"
#include "masko_store.h"

static const char *TAG = "masko_demo";

static const char *PARTITION_LABEL = "masko";
static const size_t PACK_ALIGNMENT = 512;

static void report_stats(masko_player_t *p, int64_t since, uint32_t frames_before)
{
    const masko_stats_t s = masko_player_stats(p);
    const int64_t now = esp_timer_get_time();
    const double secs = (double)(now - since) / 1e6;
    const uint32_t shown = s.frames_shown;
    const double achieved = secs > 0 ? (double)(shown - frames_before) / secs : 0.0;

    ESP_LOGI(TAG, "shown=%u dropped=%u underrun=%u | %.1f fps",
             (unsigned)shown, (unsigned)s.frames_dropped, (unsigned)s.underruns,
             achieved);
    ESP_LOGI(TAG, "decode avg %lld / max %lld us | push avg %lld / max %lld us "
                  "| serial ceiling %.1f fps",
             (long long)s.decode_us_avg, (long long)s.decode_us_max,
             (long long)s.push_us_avg, (long long)s.push_us_max,
             s.ceiling_fps);
}

static void on_event(masko_event_t ev, void *user)
{
    (void)user;
    switch (ev) {
    case MASKO_EV_CLIP_START: ESP_LOGI(TAG, "clip start"); break;
    case MASKO_EV_CLIP_END:   ESP_LOGI(TAG, "clip end");   break;
    case MASKO_EV_UNDERRUN:  ESP_LOGW(TAG, "underrun");   break;
    case MASKO_EV_ERROR:      ESP_LOGE(TAG, "playback error"); break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 BOOTED");
    ESP_LOGI(TAG, "free internal DRAM %u B, free PSRAM %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ---- panel ---------------------------------------------------------- */
    /* max_transfer_sz MUST be a band, not a frame. A whole 466x466 frame is
     * 434,312 B and the SPI master tries to allocate that much from internal
     * DRAM; with ~311 KB free it fails with ESP_ERR_NO_MEM and the screen stays
     * black. */
    const bsp_display_config_t disp_cfg = {
        .max_transfer_sz = MASKO_PUSH_BAND_BYTES,
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(bsp_display_new(&disp_cfg, &panel, &io));
    ESP_ERROR_CHECK(bsp_display_brightness_init());
    bsp_display_brightness_set(80);
    ESP_LOGI(TAG, "panel up: %dx%d, band %u B", BSP_LCD_H_RES, BSP_LCD_V_RES,
             (unsigned)MASKO_PUSH_BAND_BYTES);

    const bsp_display_cfg_t touch_cfg = {0};
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(bsp_touch_new(&touch_cfg, &touch));
    ESP_LOGI(TAG, "touch ready; tap the display to switch clips");

    /* ---- pack ----------------------------------------------------------- */
    /* masko_store hides the backend. Swap masko_src_flash() for masko_src_sd()
     * later and nothing below changes; the player only ever sees a blob. */
    masko_source_t *src = NULL;
    esp_err_t err = masko_src_flash(PARTITION_LABEL, &src);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no usable '%s' partition; flash one with "
                      "tools/flash_pack.sh", PARTITION_LABEL);
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    masko_blob_t blob;
    err = masko_source_open(src, "mango_loop.mkpk", &blob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    masko_pack_t probe;
    const int rc = masko_pack_open(blob.data, blob.len, &probe);
    if (rc != MASKO_OK) {
        ESP_LOGE(TAG, "pack rejected: %s", masko_strerror(rc));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "pack: %u frames, bbox %ux%u+%u+%u, %.1f fps",
             probe.hdr.frame_count, probe.hdr.bbox_w, probe.hdr.bbox_h,
             probe.hdr.bbox_x, probe.hdr.bbox_y, (double)masko_pack_fps(&probe));

    /* The demo bundle stores clip two at the next 512-byte boundary. */
    const size_t second_offset =
        (probe.hdr.file_size + PACK_ALIGNMENT - 1) & ~(PACK_ALIGNMENT - 1);
    masko_pack_t second_probe;
    const bool has_second = second_offset < blob.len &&
        masko_pack_open(blob.data + second_offset, blob.len - second_offset,
                        &second_probe) == MASKO_OK &&
        second_probe.hdr.bbox_w == probe.hdr.bbox_w &&
        second_probe.hdr.bbox_h == probe.hdr.bbox_h;
    if (has_second) {
        ESP_LOGI(TAG, "second clip found at flash offset %u (%u frames)",
                 (unsigned)second_offset, second_probe.hdr.frame_count);
    } else {
        ESP_LOGW(TAG, "no compatible second clip found; touch switching disabled");
    }

    /* ---- player --------------------------------------------------------- */
    masko_display_cfg_t dcfg = {
        .panel          = panel,
        .canvas_w       = BSP_LCD_H_RES,
        .canvas_h       = BSP_LCD_V_RES,
        .band_rows      = MASKO_PUSH_BAND_ROWS,
        .stats_enabled  = true,
    };
    masko_display_t *disp = NULL;
    err = masko_display_new(&dcfg, &disp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display failed: %s", esp_err_to_name(err));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* Clear anything the BSP left on screen, and cover the area outside the
     * clip's bbox with the pack background. */
    const uint16_t bg = probe.hdr.bg_rgb565;
    masko_display_fill(disp, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, bg);

    const masko_player_cfg_t pcfg = {
        .pack_data     = blob.data,
        .pack_size     = blob.len,
        .display       = disp,
        .max_fps       = CONFIG_MASKO_PACK_MAX_FPS,
        .decode_core   = 1,
        .stats_enabled = true,
        .on_event      = on_event,
    };
    masko_player_t *player = NULL;
    err = masko_player_create(&pcfg, &player);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "player create failed: %s", esp_err_to_name(err));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    err = masko_player_play(player);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "play failed: %s", esp_err_to_name(err));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "playing");

    const uint8_t *clip_data[2] = { blob.data, has_second ? blob.data + second_offset : blob.data };
    const size_t clip_size[2] = { probe.hdr.file_size, has_second ? second_probe.hdr.file_size : probe.hdr.file_size };
    unsigned active_clip = 0;
    bool touch_was_down = false;
    int64_t since = esp_timer_get_time();
    uint32_t frames_before = 0;
    while (1) {
        /* CST9217 can report an invalid ACK between touch reports. Treat that
         * as an empty sample and keep the animation task alive. */
        const esp_err_t touch_err = esp_lcd_touch_read_data(touch);
        esp_lcd_touch_point_data_t point[1];
        uint8_t points = 0;
        const bool read_ok = touch_err == ESP_OK &&
            esp_lcd_touch_get_data(touch, point, &points, 1) == ESP_OK;
        const bool touch_down = read_ok && points > 0;
        if (has_second && touch_down && !touch_was_down) {
            active_clip ^= 1;
            err = masko_player_set_pack(player, clip_data[active_clip], clip_size[active_clip]);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "touch at %u,%u: switched to clip %u",
                         (unsigned)point[0].x, (unsigned)point[0].y, active_clip + 1);
            } else {
                ESP_LOGE(TAG, "clip switch failed: %s", esp_err_to_name(err));
                active_clip ^= 1;
            }
        }
        touch_was_down = touch_down;

        if (esp_timer_get_time() - since >= 10000000) {
            report_stats(player, since, frames_before);
            since = esp_timer_get_time();
            frames_before = masko_player_stats(player).frames_shown;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
