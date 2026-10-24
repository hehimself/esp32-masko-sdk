/**
 * @file main.c
 * @brief Play a compact three-state Kylo graph on the Waveshare AMOLED 1.75.
 *
 * Panel init comes from the Waveshare BSP. Everything after that is masko_*.
 *
 * The pack lives in the `masko` flash partition and is mmapped, so it is not
 * compiled into the app binary. Flash the partition once with
 * `tools/flash_pack.sh`.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "driver/gpio.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"

#include "masko_pack.h"
#include "masko_calib.h"
#include "masko_display.h"
#include "masko_player.h"
#include "masko_store.h"
#include "masko_graph.h"

static const char *TAG = "masko_demo";

static const char *PARTITION_LABEL = "masko";
#if !CONFIG_MASKO_SINGLE_PACK_BENCHMARK
static const size_t PACK_ALIGNMENT = 512;
/* The onboard BOOT button is active-low on GPIO0. */
static const gpio_num_t REACTION_BUTTON_GPIO = GPIO_NUM_0;
#endif

enum { PACK_COUNT = 5, CLIP_COUNT = 7 };
#if !CONFIG_MASKO_SINGLE_PACK_BENCHMARK
static const char *PACK_NAMES[PACK_COUNT] = {
    "kylo-jar-gazing.loop.mkpk", "kylo-specimen-notes.loop.mkpk",
    "kylo-excited-wave.loop.mkpk", "kylo-jar-gazing_to_specimen-notes.once.mkpk",
    "kylo-jar-gazing_to_excited-wave.once.mkpk",
};
static const unsigned PACK_CLIP_INDEX[PACK_COUNT] = {0, 1, 2, 3, 5};
static const char *CLIP_NAMES[CLIP_COUNT] = {
    "kylo-jar-gazing.loop.mkpk", "kylo-specimen-notes.loop.mkpk",
    "kylo-excited-wave.loop.mkpk", "kylo-jar-gazing_to_specimen-notes.once.mkpk",
    "kylo-specimen-notes_to_jar-gazing.reverse.mkpk",
    "kylo-jar-gazing_to_excited-wave.once.mkpk",
    "kylo-excited-wave_to_jar-gazing.reverse.mkpk",
};

static bool clip_plays_reverse(unsigned clip)
{
    return clip == 4 || clip == 6;
}

static const masko_node_t GRAPH_NODES[] = {
    {"jar-gazing"}, {"specimen-notes"}, {"excited-wave"},
};
static const masko_input_t GRAPH_INPUTS[] = {
    {"want_rest", false, true, 0.0},
    {"want_working", false, false, 0.0},
    {"want_attention", false, false, 0.0},
};
static const masko_cond_t C_REST[] = {{"want_rest", MASKO_OP_EQ, false, true, 0.0}};
static const masko_cond_t C_WORKING[] = {{"want_working", MASKO_OP_EQ, false, true, 0.0}};
static const masko_cond_t C_ATTENTION[] = {{"want_attention", MASKO_OP_EQ, false, true, 0.0}};
static const masko_edge_t GRAPH_EDGES[] = {
    {"jar-gazing", "jar-gazing", true, "kylo-jar-gazing.loop.mkpk", 0, NULL, 0},
    {"jar-gazing", "specimen-notes", false,
        "kylo-jar-gazing_to_specimen-notes.once.mkpk", 2000, C_WORKING, 1},
    {"jar-gazing", "excited-wave", false,
        "kylo-jar-gazing_to_excited-wave.once.mkpk", 2000, C_ATTENTION, 1},
    {"specimen-notes", "specimen-notes", true,
        "kylo-specimen-notes.loop.mkpk", 0, NULL, 0},
    {"specimen-notes", "jar-gazing", false,
        "kylo-specimen-notes_to_jar-gazing.reverse.mkpk", 2000, C_REST, 1},
    {"excited-wave", "excited-wave", true,
        "kylo-excited-wave.loop.mkpk", 0, NULL, 0},
    {"excited-wave", "jar-gazing", false,
        "kylo-excited-wave_to_jar-gazing.reverse.mkpk", 2000, C_REST, 1},
};
static const masko_graph_t DEMO_GRAPH = {
    GRAPH_NODES, 3, GRAPH_EDGES, 7, GRAPH_INPUTS, 3, "jar-gazing", true,
};
#endif

typedef struct {
    masko_player_t *player;
    masko_graph_rt_t graph_rt;
    SemaphoreHandle_t graph_lock;
    const uint8_t *clip_data[CLIP_COUNT];
    size_t clip_size[CLIP_COUNT];
    unsigned active_clip;
    unsigned pending_clip;
    bool has_pending_clip;
    unsigned next_target;
    unsigned route_target;
    bool has_route_target;
} demo_state_t;

static demo_state_t demo;

#if !CONFIG_MASKO_SINGLE_PACK_BENCHMARK
static void set_graph_target(masko_graph_rt_t *rt, unsigned target)
{
    masko_graph_set_bool(rt, "want_rest", target == 0);
    masko_graph_set_bool(rt, "want_working", target == 1);
    masko_graph_set_bool(rt, "want_attention", target == 2);
}

static int clip_index(const char *name)
{
    for (int i = 0; i < CLIP_COUNT; i++)
        if (strcmp(name, CLIP_NAMES[i]) == 0) return i;
    return -1;
}
#endif

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

#if !CONFIG_MASKO_SINGLE_PACK_BENCHMARK
static void on_event(masko_event_t ev, void *user)
{
    demo_state_t *state = (demo_state_t *)user;
    switch (ev) {
    case MASKO_EV_CLIP_START:
        xSemaphoreTake(state->graph_lock, portMAX_DELAY);
        if (state->has_pending_clip) {
            state->active_clip = state->pending_clip;
            state->has_pending_clip = false;
        }
        const unsigned active = state->active_clip;
        xSemaphoreGive(state->graph_lock);
        ESP_LOGI(TAG, "clip start: %s", CLIP_NAMES[active]);
        break;
    case MASKO_EV_CLIP_END: {
        /* Graph decisions occur only at real clip boundaries. */
        masko_step_t step = {0};
        xSemaphoreTake(state->graph_lock, portMAX_DELAY);
        const unsigned active = state->active_clip;
        /* Working and Attention have authored edges only to Rest. If the
         * requested destination is the other state, take that edge as soon
         * as the reverse transition lands at Rest. */
        if (state->has_route_target && (active == 4 || active == 6) &&
            state->graph_rt.current == 0) {
            set_graph_target(&state->graph_rt, state->route_target);
            state->has_route_target = false;
        }
        int rc = masko_graph_next(&state->graph_rt, &step);
        /* If the requested state changed while a transition was playing,
         * don't force a complete loop of the transition's landing state.
         * Immediately take the newly requested authored transition instead. */
        if (rc == MASKO_GRAPH_OK && step.edge && step.edge->is_loop &&
            active >= 3 && masko_graph_match(&state->graph_rt,
                                              state->graph_rt.current) != NULL) {
            rc = masko_graph_next(&state->graph_rt, &step);
        }
        const int next = (rc == MASKO_GRAPH_OK && step.edge) ?
                         clip_index(step.edge->clip) : -1;
        const bool already_prepared = next >= 0 && state->has_pending_clip &&
                                      state->pending_clip == (unsigned)next;
        if (next < 0) {
            xSemaphoreGive(state->graph_lock);
            ESP_LOGE(TAG, "graph has no playable next clip (rc=%d)", rc);
            break;
        }
        if (already_prepared || (unsigned)next == active) {
            xSemaphoreGive(state->graph_lock);
            break;
        }
        /* Keep graph and demo handoff state serialized with touch handling.
         * In particular, do not let a touch race this fallback queue attempt. */
        const esp_err_t err = masko_player_set_pack_direction(
            state->player, state->clip_data[next], state->clip_size[next],
            clip_plays_reverse((unsigned)next));
        if (err == ESP_OK) {
            state->pending_clip = (unsigned)next;
            state->has_pending_clip = true;
            ESP_LOGI(TAG, "prepared graph clip: %s", CLIP_NAMES[next]);
        } else {
            ESP_LOGE(TAG, "cannot prepare %s: %s", CLIP_NAMES[next],
                     esp_err_to_name(err));
        }
        xSemaphoreGive(state->graph_lock);
        break;
    }
    case MASKO_EV_UNDERRUN:  ESP_LOGW(TAG, "underrun");   break;
    case MASKO_EV_ERROR:      ESP_LOGE(TAG, "playback error"); break;
    }
}
#endif

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
#if CONFIG_MASKO_SINGLE_PACK_BENCHMARK
    ESP_LOGI(TAG, "single-pack benchmark mode: touch and BOOT state switching disabled");
#else
    ESP_LOGI(TAG, "touch and BOOT ready; Kylo cycles Rest -> Working -> Attention");
#endif

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
    err = masko_source_open(src, "mascot_bundle.mkpk", &blob);
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
    if (probe.hdr.canvas_w != BSP_LCD_H_RES ||
        probe.hdr.canvas_h != BSP_LCD_V_RES ||
        (probe.hdr.flags & MASKO_FLAG_LOOP) == 0
#if !CONFIG_MASKO_SINGLE_PACK_BENCHMARK
        || probe.hdr.bbox_w != 384 || probe.hdr.bbox_h != 384 ||
           probe.hdr.bbox_x != 40 || probe.hdr.bbox_y != 40
#endif
        ) {
#if CONFIG_MASKO_SINGLE_PACK_BENCHMARK
        ESP_LOGE(TAG, "benchmark pack must be a loop matching the display canvas");
#else
        ESP_LOGE(TAG, "first Kylo pack must be a 384x384 loop centered on the 466 canvas");
#endif
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "pack: %u frames, bbox %ux%u+%u+%u, %.1f fps",
             probe.hdr.frame_count, probe.hdr.bbox_w, probe.hdr.bbox_h,
             probe.hdr.bbox_x, probe.hdr.bbox_y, (double)masko_pack_fps(&probe));

    /* Bundle order is CLIP_NAMES above; every offset is aligned for mmap reads. */
    demo.clip_data[0] = blob.data;
    demo.clip_size[0] = probe.hdr.file_size;
#if !CONFIG_MASKO_SINGLE_PACK_BENCHMARK
    size_t offset = (probe.hdr.file_size + PACK_ALIGNMENT - 1) & ~(PACK_ALIGNMENT - 1);
    bool bundle_ok = true;
    for (unsigned i = 1; i < PACK_COUNT; i++) {
        masko_pack_t clip;
        if (offset >= blob.len ||
            masko_pack_open(blob.data + offset, blob.len - offset, &clip) != MASKO_OK ||
            clip.hdr.canvas_w != probe.hdr.canvas_w ||
            clip.hdr.canvas_h != probe.hdr.canvas_h ||
            clip.hdr.bbox_x != probe.hdr.bbox_x ||
            clip.hdr.bbox_y != probe.hdr.bbox_y ||
            clip.hdr.bbox_w != probe.hdr.bbox_w ||
            clip.hdr.bbox_h != probe.hdr.bbox_h ||
            (((clip.hdr.flags & MASKO_FLAG_LOOP) != 0) != (i < 3))) {
            ESP_LOGE(TAG, "bundle missing or incompatible clip: %s", PACK_NAMES[i]);
            bundle_ok = false;
            break;
        }
        const unsigned slot = PACK_CLIP_INDEX[i];
        demo.clip_data[slot] = blob.data + offset;
        demo.clip_size[slot] = clip.hdr.file_size;
        ESP_LOGI(TAG, "bundle clip %s: %u frames", PACK_NAMES[i], clip.hdr.frame_count);
        offset += (clip.hdr.file_size + PACK_ALIGNMENT - 1) & ~(PACK_ALIGNMENT - 1);
    }
    if (!bundle_ok) {
        ESP_LOGE(TAG, "flash the five Kylo packs in the documented bundle order");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    /* Return edges reuse the two forward assets in reverse. */
    demo.clip_data[4] = demo.clip_data[3];
    demo.clip_size[4] = demo.clip_size[3];
    demo.clip_data[6] = demo.clip_data[5];
    demo.clip_size[6] = demo.clip_size[5];
#endif

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
        .pack_size     = demo.clip_size[0],
        .display       = disp,
        .max_fps       = CONFIG_MASKO_PACK_MAX_FPS,
        .decode_core   = 1,
        .stats_enabled = true,
#if CONFIG_MASKO_SINGLE_PACK_BENCHMARK
        .on_event      = NULL,
#else
        .on_event      = on_event,
#endif
        .user          = &demo,
    };
    masko_player_t *player = NULL;
    err = masko_player_create(&pcfg, &player);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "player create failed: %s", esp_err_to_name(err));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    demo.player = player;
    demo.graph_lock = xSemaphoreCreateMutex();
#if CONFIG_MASKO_SINGLE_PACK_BENCHMARK
    demo.graph_lock = NULL;
#else
    if (demo.graph_lock == NULL ||
        masko_graph_init(&demo.graph_rt, &DEMO_GRAPH, NULL, NULL, 0) != MASKO_GRAPH_OK) {
        ESP_LOGE(TAG, "graph runtime initialization failed");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    demo.active_clip = 0;
    demo.next_target = 1;
    demo.has_route_target = false;
#endif

    err = masko_player_play(player);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "play failed: %s", esp_err_to_name(err));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "playing");

#if CONFIG_MASKO_SINGLE_PACK_BENCHMARK
    ESP_LOGI(TAG, "single-pack benchmark active; leave this pack running for 30-second samples");
    int64_t since = esp_timer_get_time();
    uint32_t frames_before = 0;
    while (1) {
        if (esp_timer_get_time() - since >= 30000000) {
            report_stats(player, since, frames_before);
            since = esp_timer_get_time();
            frames_before = masko_player_stats(player).frames_shown;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#else

    const gpio_config_t button_cfg = {
        .pin_bit_mask = 1ULL << REACTION_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_cfg));
    bool input_was_down = false;
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
        const bool button_down = gpio_get_level(REACTION_BUTTON_GPIO) == 0;
        const bool input_down = touch_down || button_down;
        if (input_down && !input_was_down) {
            const char *input_name = touch_down ? "touch" : "BOOT button";
            xSemaphoreTake(demo.graph_lock, portMAX_DELAY);
            if (demo.has_pending_clip || demo.active_clip >= 3 || demo.graph_rt.landed) {
                ESP_LOGI(TAG, "%s ignored while a transition is playing", input_name);
                xSemaphoreGive(demo.graph_lock);
                input_was_down = input_down;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            const unsigned old_next_target = demo.next_target;
            const unsigned old_route_target = demo.route_target;
            const bool old_has_route_target = demo.has_route_target;
            const int old_node = demo.graph_rt.current;
            const bool old_landed = demo.graph_rt.landed;
            const unsigned current = (unsigned)demo.graph_rt.current;
            const unsigned target = demo.next_target;
            demo.next_target = (demo.next_target + 1) % 3;
            demo.has_route_target = current != 0 && target != 0;
            demo.route_target = target;
            set_graph_target(&demo.graph_rt, demo.has_route_target ? 0 : target);

            /* Prepare and commit the requested edge now, then hand off on the
             * next frame. Long loop clips no longer delay touch reactions. */
            const masko_edge_t *edge = masko_graph_match(&demo.graph_rt,
                                                         demo.graph_rt.current);
            if (edge != NULL) {
                masko_step_t step = {0};
                const int graph_rc = masko_graph_next(&demo.graph_rt, &step);
                const int candidate = graph_rc == MASKO_GRAPH_OK && step.edge == edge ?
                                      clip_index(edge->clip) : -1;
                if (candidate >= 3) {
                    err = masko_player_set_pack_direction_immediate(
                        player, demo.clip_data[candidate], demo.clip_size[candidate],
                        clip_plays_reverse((unsigned)candidate));
                    if (err == ESP_OK) {
                        demo.pending_clip = (unsigned)candidate;
                        demo.has_pending_clip = true;
                    } else {
                        demo.graph_rt.current = old_node;
                        demo.graph_rt.landed = old_landed;
                        demo.next_target = old_next_target;
                        demo.route_target = old_route_target;
                        demo.has_route_target = old_has_route_target;
                        set_graph_target(&demo.graph_rt, current);
                        ESP_LOGE(TAG, "transition preflight failed: %s",
                                 esp_err_to_name(err));
                    }
                }
            }
            xSemaphoreGive(demo.graph_lock);
            const char *target_name = target == 0 ? "Rest" :
                                      target == 1 ? "Working" : "Attention";
            if (touch_down) {
                ESP_LOGI(TAG, "%s at %u,%u: target %s", input_name,
                         (unsigned)point[0].x, (unsigned)point[0].y,
                         target_name);
            } else {
                ESP_LOGI(TAG, "%s pressed: target %s", input_name,
                         target_name);
            }
        }
        input_was_down = input_down;

        if (esp_timer_get_time() - since >= 30000000) {
            report_stats(player, since, frames_before);
            since = esp_timer_get_time();
            frames_before = masko_player_stats(player).frames_shown;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#endif
}
