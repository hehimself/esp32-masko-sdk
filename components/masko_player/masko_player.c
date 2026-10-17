/**
 * @file masko_player.c
 * @brief Two-task playback: decode on core 1, push on core 0, double buffered.
 */
#include "masko_player.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "masko_codec.h"
#include "masko_calib.h"

static const char *TAG = "masko_player";

#define DECODE_TASK_STACK 4096
#define DECODE_TASK_PRIO  6
#define PUSH_TASK_STACK   4096
#define PUSH_TASK_PRIO    7

typedef struct {
    uint8_t         *buf[2];      /* PSRAM, 16-byte aligned */
    uint16_t         w, h;

    QueueHandle_t    ready;       /* buffer indices decoded and awaiting push */
    SemaphoreHandle_t freed;      /* signalled once the presenter is done */

    volatile bool    running;
    int8_t           decode_core;
    masko_decoder_t *decoder;
} swap_t;

struct masko_player_t {
    masko_pack_t     pack;
    masko_display_t *display;
    swap_t           swap;

    masko_event_cb_t on_event;
    void            *user;
    bool             stats_enabled;

    TaskHandle_t decode_task;
    TaskHandle_t push_task;

    volatile uint32_t frames_shown;
    volatile uint32_t frames_dropped;
    volatile uint32_t underruns;
    volatile int64_t  decode_us_max;
    volatile int64_t  push_us_max;
    volatile int64_t  decode_us_total;
    volatile int64_t  push_us_total;
    volatile int64_t  first_frame_us;
};

/* --------------------------------------------------------------- helpers */

static void sleep_until(int64_t delay_us)
{
    if (delay_us <= 0) {
        return;
    }
    const int64_t start = esp_timer_get_time();
    if (delay_us > 2000) {
        vTaskDelay(pdMS_TO_TICKS((delay_us - 1000) / 1000));
    }
    const int64_t elapsed = esp_timer_get_time() - start;
    if (elapsed < delay_us) {
        esp_rom_delay_us((uint32_t)(delay_us - elapsed));
    }
}

/* --------------------------------------------------------- decode task --- */
/* Runs on core 1. CPU-bound in esp_jpeg; never touches the panel. */

static void decode_task(void *arg)
{
    masko_player_t *p = (masko_player_t *)arg;
    swap_t *sw = &p->swap;

    const float fps = masko_pack_fps(&p->pack);
    const int64_t period_us = fps > 0.0f ? (int64_t)(1e6f / fps) : 41666;
    const size_t need = masko_frame_bytes(sw->w, sw->h);

    uint32_t idx = 0;
    int spare = 0;
    int64_t next_due = esp_timer_get_time() + period_us;   /* one frame of lead */

    while (sw->running) {
        const uint8_t *jpg = NULL;
        uint32_t jlen = 0;
        if (masko_pack_frame(&p->pack, idx, &jpg, &jlen) != MASKO_OK) {
            p->frames_dropped++;
            idx = (idx + 1) % p->pack.hdr.frame_count;
            continue;
        }

        /* Block until the presenter releases this buffer. */
        if (xSemaphoreTake(sw->freed, pdMS_TO_TICKS(500)) != pdTRUE) {
            if (!sw->running) break;
        }

        const int64_t t0 = esp_timer_get_time();
        const uint8_t *out = NULL;
        uint16_t w = 0, h = 0;
        const esp_err_t err = masko_decoder_decode(sw->decoder, jpg, jlen,
                                                    sw->buf[spare], need, &out, &w, &h);
        const int64_t dt = esp_timer_get_time() - t0;

        if (err != ESP_OK) {
            p->frames_dropped++;
            if (p->on_event) p->on_event(MASKO_EV_ERROR, p->user);
        } else {
            if (dt > p->decode_us_max) p->decode_us_max = dt;
            p->decode_us_total += dt;
            if (p->first_frame_us == 0) p->first_frame_us = esp_timer_get_time();
            xQueueSend(sw->ready, &spare, pdMS_TO_TICKS(500));
            spare = 1 - spare;
        }

        idx = (idx + 1) % p->pack.hdr.frame_count;

        /* Pace against an absolute schedule so rounding cannot accumulate drift. */
        next_due += period_us;
        const int64_t now = esp_timer_get_time();
        if (next_due > now) {
            sleep_until(next_due - now);
        } else {
            next_due = now;   /* behind: resync instead of bursting */
        }
    }

    sw->running = false;
    vTaskDelete(NULL);
}

/* ----------------------------------------------------------- push task --- */
/* Runs on core 0. Blocks in the SPI DMA engine, leaving the CPU free for the
 * decoder on the other core. This is where the overlap comes from. */

static void push_task(void *arg)
{
    masko_player_t *p = (masko_player_t *)arg;
    swap_t *sw = &p->swap;

    uint32_t shown = 0;

    while (sw->running) {
        int buf = -1;
        if (xQueueReceive(sw->ready, &buf, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (!sw->running) break;
            p->underruns++;
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = masko_display_blit(p->display,
                                                  p->pack.hdr.bbox_x, p->pack.hdr.bbox_y,
                                                  sw->w, sw->h, sw->buf[buf]);
        const int64_t dt = esp_timer_get_time() - t0;

        if (err == ESP_OK) {
            if (dt > p->push_us_max) p->push_us_max = dt;
            p->push_us_total += dt;
            shown++;
            p->frames_shown = shown;
        } else {
            p->frames_dropped++;
            if (p->on_event) p->on_event(MASKO_EV_ERROR, p->user);
        }

        xSemaphoreGive(sw->freed);   /* let the decoder reuse this buffer */
    }

    sw->running = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------ lifecycle - */

esp_err_t masko_player_create(const masko_player_cfg_t *cfg, masko_player_t **out)
{
    if (cfg == NULL || out == NULL || cfg->display == NULL || cfg->pack_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    masko_player_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int rc = masko_pack_open(cfg->pack_data, cfg->pack_size, &p->pack);
    if (rc != MASKO_OK) {
        ESP_LOGE(TAG, "pack rejected: %s", masko_strerror(rc));
        free(p);
        return ESP_ERR_INVALID_VERSION;
    }

    p->display = cfg->display;
    p->on_event = cfg->on_event;
    p->user = cfg->user;
    p->stats_enabled = cfg->stats_enabled;

    swap_t *sw = &p->swap;
    sw->w = p->pack.hdr.bbox_w;
    sw->h = p->pack.hdr.bbox_h;
    sw->decode_core = cfg->decode_core;

    const size_t need = masko_frame_bytes(sw->w, sw->h);
    for (int i = 0; i < 2; i++) {
        sw->buf[i] = heap_caps_aligned_alloc(MASKO_DECODE_BUF_ALIGN, need,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (sw->buf[i] == NULL) {
            ESP_LOGE(TAG, "cannot allocate decode buffer %d (%u B)", i, (unsigned)need);
            for (int j = 0; j < i; j++) heap_caps_free(sw->buf[j]);
            free(p);
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = masko_decoder_new(sw->w, sw->h, &sw->decoder);
    if (err != ESP_OK) {
        heap_caps_free(sw->buf[0]);
        heap_caps_free(sw->buf[1]);
        free(p);
        return err;
    }

    sw->ready = xQueueCreate(2, sizeof(int));
    sw->freed = xSemaphoreCreateBinary();
    if (sw->ready == NULL || sw->freed == NULL) {
        ESP_LOGE(TAG, "cannot create sync primitives");
        masko_decoder_free(sw->decoder);
        heap_caps_free(sw->buf[0]);
        heap_caps_free(sw->buf[1]);
        free(p);
        return ESP_ERR_NO_MEM;
    }
    /* Start with both buffers free. */
    xSemaphoreGive(sw->freed);
    xSemaphoreGive(sw->freed);

    ESP_LOGI(TAG, "pack: %u frames, bbox %ux%u+%u+%u, %.1f fps, %.1f ms decode (measured)",
             p->pack.hdr.frame_count, sw->w, sw->h, p->pack.hdr.bbox_x, p->pack.hdr.bbox_y,
             (double)masko_pack_fps(&p->pack),
             (double)sw->w * sw->h / (1e6 * MASKO_DECODE_MPX_S));

    *out = p;
    return ESP_OK;
}

esp_err_t masko_player_play(masko_player_t *p)
{
    if (p == NULL || p->decode_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    p->swap.running = true;
    p->frames_shown = 0;
    p->frames_dropped = 0;
    p->underruns = 0;
    p->decode_us_max = p->push_us_max = 0;
    p->decode_us_total = p->push_us_total = 0;
    p->first_frame_us = 0;
    xQueueReset(p->swap.ready);

    BaseType_t ok = xTaskCreatePinnedToCore(push_task, "masko_push", PUSH_TASK_STACK,
                                            p, PUSH_TASK_PRIO, &p->push_task, 0);
    if (ok != pdPASS) {
        p->swap.running = false;
        return ESP_ERR_NO_MEM;
    }
    if (p->swap.decode_core < 0) {
        ok = xTaskCreate(decode_task, "masko_dec", DECODE_TASK_STACK, p,
                         DECODE_TASK_PRIO, &p->decode_task);
    } else {
        ok = xTaskCreatePinnedToCore(decode_task, "masko_dec", DECODE_TASK_STACK, p,
                                     DECODE_TASK_PRIO, &p->decode_task,
                                     (BaseType_t)p->swap.decode_core);
    }
    if (ok != pdPASS) {
        p->swap.running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void masko_player_stop(masko_player_t *p)
{
    if (p == NULL) {
        return;
    }
    p->swap.running = false;
    for (int i = 0; i < 100 && p->swap.running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    p->decode_task = NULL;
    p->push_task = NULL;
}

void masko_player_destroy(masko_player_t *p)
{
    if (p == NULL) {
        return;
    }
    masko_player_stop(p);
    if (p->swap.ready)  vQueueDelete(p->swap.ready);
    if (p->swap.freed)  vSemaphoreDelete(p->swap.freed);
    if (p->swap.decoder) masko_decoder_free(p->swap.decoder);
    if (p->swap.buf[0]) heap_caps_free(p->swap.buf[0]);
    if (p->swap.buf[1]) heap_caps_free(p->swap.buf[1]);
    free(p);
}

masko_stats_t masko_player_stats(const masko_player_t *p)
{
    masko_stats_t s;
    memset(&s, 0, sizeof(s));
    if (p == NULL) {
        return s;
    }
    s.frames_shown    = p->frames_shown;
    s.frames_dropped  = p->frames_dropped;
    s.underruns       = p->underruns;
    s.decode_us_max   = p->decode_us_max;
    s.push_us_max     = p->push_us_max;
    if (p->frames_shown > 0) {
        s.decode_us_avg = p->decode_us_total / p->frames_shown;
        s.push_us_avg   = p->push_us_total / p->frames_shown;
        const int64_t mean = s.decode_us_avg + s.push_us_avg;
        if (mean > 0) {
            s.ceiling_fps = 1e6 / (double)mean;
        }
    }
    s.achieved_fps = (double)p->pack.hdr.fps_num /
                     ((double)p->pack.hdr.fps_den ? (double)p->pack.hdr.fps_den : 1.0);
    return s;
}