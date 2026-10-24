#include "masko_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "masko_http";

typedef struct {
    uint8_t *dst;
    size_t want, got;
    uint32_t start, end, total;
    bool range_seen, overflow;
} transfer_t;

typedef struct {
    uint8_t *data;
    uint32_t first, last, offset, length;
    bool ready, busy;
} slot_t;

struct masko_http_t {
    QueueHandle_t jobs;
    SemaphoreHandle_t lock;
    TaskHandle_t worker;
    SemaphoreHandle_t worker_done;
    size_t cache_bytes;
    volatile bool running;
    masko_http_stats_t stats;
};

struct masko_http_clip_t {
    masko_http_t *http;
    char *url;
    masko_frame_source_t source;
    uint32_t *offsets, *lengths;
    slot_t slot[2];
    unsigned jobs_outstanding;
    bool closing;
};

typedef struct {
    masko_http_clip_t *clip;
    uint8_t slot;
    uint32_t first, last, offset, length;
} job_t;

static esp_err_t on_http(esp_http_client_event_t *ev)
{
    transfer_t *t = ev->user_data;
    if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key &&
        strcasecmp(ev->header_key, "Content-Range") == 0) {
        unsigned a, b, total;
        if (sscanf(ev->header_value, "bytes %u-%u/%u", &a, &b, &total) == 3) {
            t->start = a; t->end = b; t->total = total; t->range_seen = true;
        }
    } else if (ev->event_id == HTTP_EVENT_ON_DATA && ev->data_len > 0) {
        if ((size_t)ev->data_len > t->want - t->got) {
            t->overflow = true;
        } else {
            memcpy(t->dst + t->got, ev->data, (size_t)ev->data_len);
            t->got += (size_t)ev->data_len;
        }
    }
    return ESP_OK;
}

static esp_http_client_handle_t client_new(const char *url, transfer_t *t)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http,
        .user_data = t,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    return esp_http_client_init(&cfg);
}

static esp_err_t read_range(esp_http_client_handle_t client, transfer_t *t,
                            const char *url, uint32_t start, size_t len,
                            uint8_t *dst, uint32_t *total)
{
    if (!client || !t || !url || !dst || !len || len > UINT32_MAX - start)
        return ESP_ERR_INVALID_ARG;
    memset(t, 0, sizeof(*t));
    t->dst = dst; t->want = len;
    char range[64];
    snprintf(range, sizeof(range), "bytes=%u-%u", (unsigned)start,
             (unsigned)(start + (uint32_t)len - 1));
    esp_err_t err = esp_http_client_set_url(client, url);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Range", range);
    if (err != ESP_OK) return err;
    err = esp_http_client_perform(client);
    if (err != ESP_OK) return err;
    if (esp_http_client_get_status_code(client) != 206 || !t->range_seen ||
        t->overflow || t->got != len || t->start != start ||
        t->end != start + len - 1 || t->total < t->end + 1) {
        ESP_LOGE(TAG, "bad range response: status=%d range=%u-%u/%u got=%u want=%u",
                 esp_http_client_get_status_code(client), t->start, t->end,
                 t->total, (unsigned)t->got, (unsigned)len);
        return ESP_FAIL;
    }
    if (total) *total = t->total;
    return ESP_OK;
}

static void worker(void *arg)
{
    masko_http_t *http = arg;
    transfer_t transfer = {0};
    esp_http_client_handle_t client = NULL;
    while (http->running) {
        job_t job;
        if (xQueueReceive(http->jobs, &job, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (!job.clip) continue;
        if (!client) client = client_new(job.clip->url, &transfer);
        const int64_t start = esp_timer_get_time();
        uint32_t total = 0;
        esp_err_t err = client ? read_range(client, &transfer, job.clip->url,
                                            job.offset, job.length,
                                            job.clip->slot[job.slot].data, &total)
                               : ESP_ERR_NO_MEM;
        if (err != ESP_OK || total != job.clip->source.header.file_size) {
            if (client) esp_http_client_cleanup(client);
            client = NULL;
            /* A fresh connection handles a stale keep-alive socket. */
            client = client_new(job.clip->url, &transfer);
            err = client ? read_range(client, &transfer, job.clip->url,
                                      job.offset, job.length,
                                      job.clip->slot[job.slot].data, &total)
                         : ESP_ERR_NO_MEM;
            if (err == ESP_OK && total != job.clip->source.header.file_size)
                err = ESP_ERR_INVALID_SIZE;
        }
        const uint32_t ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
        xSemaphoreTake(http->lock, portMAX_DELAY);
        slot_t *s = &job.clip->slot[job.slot];
        if (err == ESP_OK) {
            s->first = job.first; s->last = job.last;
            s->offset = job.offset; s->length = job.length;
            s->ready = true;
            http->stats.bytes += job.length;
        } else {
            s->ready = false;
            http->stats.failures++;
            ESP_LOGW(TAG, "range failed for %s: %s", job.clip->url,
                     esp_err_to_name(err));
        }
        s->busy = false;
        http->stats.requests++;
        if (ms > http->stats.peak_request_ms) http->stats.peak_request_ms = ms;
        job.clip->jobs_outstanding--;
        xSemaphoreGive(http->lock);
    }
    if (client) esp_http_client_cleanup(client);
    xSemaphoreGive(http->worker_done);
    vTaskDelete(NULL);
}

esp_err_t masko_http_new(size_t cache_bytes, masko_http_t **out)
{
    if (!out || cache_bytes < 65536 || cache_bytes > 1024 * 1024)
        return ESP_ERR_INVALID_ARG;
    masko_http_t *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    h->cache_bytes = cache_bytes;
    h->jobs = xQueueCreate(8, sizeof(job_t));
    h->lock = xSemaphoreCreateMutex();
    h->worker_done = xSemaphoreCreateBinary();
    if (!h->jobs || !h->lock || !h->worker_done) {
        if (h->jobs) vQueueDelete(h->jobs);
        if (h->lock) vSemaphoreDelete(h->lock);
        if (h->worker_done) vSemaphoreDelete(h->worker_done);
        free(h); return ESP_ERR_NO_MEM;
    }
    h->running = true;
    if (xTaskCreatePinnedToCore(worker, "masko_http", 6144, h, 4,
                                &h->worker, 0) != pdPASS) {
        h->running = false; vQueueDelete(h->jobs); vSemaphoreDelete(h->lock);
        vSemaphoreDelete(h->worker_done);
        free(h); return ESP_ERR_NO_MEM;
    }
    *out = h;
    return ESP_OK;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool enqueue(masko_http_clip_t *c, unsigned slot, uint32_t at, bool reverse)
{
    masko_http_t *h = c->http;
    if (slot > 1 || at >= c->source.header.frame_count ||
        c->slot[slot].busy || c->closing) return false;
    uint32_t lo = at, hi = at;
    const uint32_t base = c->offsets[at];
    /* A cold scene should not wait for a full 512-KiB cache fill before its
     * first frame can be displayed. Later requests use the full cache. */
    const bool cold = !c->slot[0].ready && !c->slot[1].ready;
    const uint32_t limit = cold ?
        (c->lengths[at] > 64 * 1024 ? c->lengths[at] :
         (h->cache_bytes < 64 * 1024 ? h->cache_bytes : 64 * 1024)) :
        h->cache_bytes;
    if (reverse) {
        while (lo > 0 && base + c->lengths[at] - c->offsets[lo-1] <= limit)
            lo--;
    } else {
        while (hi + 1 < c->source.header.frame_count &&
               c->offsets[hi+1] + c->lengths[hi+1] - base <= limit)
            hi++;
    }
    job_t job = { c, (uint8_t)slot, lo, hi, c->offsets[lo],
                  c->offsets[hi] + c->lengths[hi] - c->offsets[lo] };
    if (job.length > h->cache_bytes) return false;
    c->slot[slot].busy = true;
    c->slot[slot].ready = false;
    c->slot[slot].first = lo;
    c->slot[slot].last = hi;
    if (xQueueSend(h->jobs, &job, 0) != pdTRUE) {
        c->slot[slot].busy = false;
        return false;
    }
    c->jobs_outstanding++;
    return true;
}

static esp_err_t frame_read(void *context, uint32_t frame, bool reverse,
                            uint8_t *dst, size_t cap, uint32_t *length,
                            uint32_t timeout_ms)
{
    masko_http_clip_t *c = context;
    if (!c || !dst || !length || frame >= c->source.header.frame_count ||
        c->lengths[frame] > cap) return ESP_ERR_INVALID_ARG;
    masko_http_t *h = c->http;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    bool miss_counted = false;
    do {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        for (unsigned i = 0; i < 2; i++) {
            slot_t *s = &c->slot[i];
            if (s->ready && frame >= s->first && frame <= s->last) {
                const uint32_t pos = c->offsets[frame] - s->offset;
                memcpy(dst, s->data + pos, c->lengths[frame]);
                *length = c->lengths[frame];
                h->stats.cache_hits++;
                /* Start the next range as soon as this chunk is consumed.
                 * A full chunk of lead time is needed on marginal Wi-Fi. */
                {
                    const uint32_t next = reverse ?
                        (s->first ? s->first - 1 : c->source.header.frame_count-1) :
                        (s->last+1 < c->source.header.frame_count ? s->last+1 : 0);
                    const bool wraps = reverse ? s->first == 0 :
                                        s->last+1 >= c->source.header.frame_count;
                    if (next < c->source.header.frame_count &&
                        (!wraps || (c->source.header.flags & MASKO_FLAG_LOOP))) {
                        const unsigned other = 1 - i;
                        if (!c->slot[other].busy &&
                            !(c->slot[other].ready && next >= c->slot[other].first &&
                              next <= c->slot[other].last))
                            enqueue(c, other, next, reverse);
                    }
                }
                xSemaphoreGive(h->lock);
                if (dst[0] != 0xff || dst[1] != 0xd8 ||
                    dst[*length-2] != 0xff || dst[*length-1] != 0xd9)
                    return ESP_ERR_INVALID_RESPONSE;
                return ESP_OK;
            }
        }
        if (!miss_counted) { h->stats.cache_misses++; miss_counted = true; }
        bool already = false;
        for (unsigned i = 0; i < 2; i++) {
            slot_t *s = &c->slot[i];
            if (s->busy && frame >= s->first && frame <= s->last) already = true;
        }
        if (!already) {
            unsigned replace = c->slot[0].busy ? 1 : 0;
            if (!c->slot[replace].busy) enqueue(c, replace, frame, reverse);
        }
        xSemaphoreGive(h->lock);
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

esp_err_t masko_http_clip_open(masko_http_t *http, const char *url,
                               masko_http_clip_t **out)
{
    if (!http || !url || !out || strncmp(url, "https://", 8) != 0)
        return ESP_ERR_INVALID_ARG;
    masko_http_clip_t *c = calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    c->http = http;
    c->url = strdup(url);
    if (!c->url) { free(c); return ESP_ERR_NO_MEM; }
    transfer_t t = {0};
    esp_http_client_handle_t client = client_new(url, &t);
    uint8_t hdr[MASKO_HEADER_SIZE];
    uint32_t total = 0;
    const int64_t header_start = esp_timer_get_time();
    esp_err_t err = client ? read_range(client, &t, url, 0, sizeof(hdr), hdr, &total)
                           : ESP_ERR_NO_MEM;
    const uint32_t header_ms = (uint32_t)((esp_timer_get_time()-header_start)/1000);
    if (err == ESP_OK && masko_pack_read_header(hdr, sizeof(hdr),
                                                &c->source.header) != MASKO_OK)
        err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && total != c->source.header.file_size)
        err = ESP_ERR_INVALID_SIZE;
    const uint32_t count = c->source.header.frame_count;
    if (err == ESP_OK && count > 10000) err = ESP_ERR_INVALID_SIZE;
    uint8_t *index = NULL;
    uint32_t index_ms = 0;
    if (err == ESP_OK) {
        const size_t n = (size_t)count * 8;
        index = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!index) err = ESP_ERR_NO_MEM;
        else {
            const int64_t index_start = esp_timer_get_time();
            err = read_range(client, &t, url, c->source.header.index_offset,
                             n, index, &total);
            index_ms = (uint32_t)((esp_timer_get_time()-index_start)/1000);
            if (err == ESP_OK && total != c->source.header.file_size)
                err = ESP_ERR_INVALID_SIZE;
        }
    }
    if (client) esp_http_client_cleanup(client);
    if (err == ESP_OK) {
        c->offsets = heap_caps_malloc((size_t)count * sizeof(uint32_t) * 2,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!c->offsets) err = ESP_ERR_NO_MEM;
        else c->lengths = c->offsets + count;
    }
    uint32_t max_frame = 0;
    for (uint32_t i = 0; err == ESP_OK && i < count; i++) {
        const uint32_t off = rd32(index + i*8), n = rd32(index + i*8+4);
        if (n < 4 || off % MASKO_PAYLOAD_ALIGN || off < c->source.header.data_offset ||
            off > c->source.header.file_size || n > c->source.header.file_size-off ||
            n > http->cache_bytes || (i && off <= c->offsets[i-1])) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        c->offsets[i] = off; c->lengths[i] = n;
        if (n > max_frame) max_frame = n;
    }
    if (index) heap_caps_free(index);
    if (err == ESP_OK) {
        for (unsigned i = 0; i < 2; i++) {
            c->slot[i].data = heap_caps_malloc(http->cache_bytes,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!c->slot[i].data) { err = ESP_ERR_NO_MEM; break; }
        }
    }
    if (err != ESP_OK) {
        masko_http_clip_close(c);
        return err;
    }
    c->source.context = c;
    c->source.max_frame_bytes = max_frame;
    c->source.frame_step = 1;
    c->source.read_frame = frame_read;
    xSemaphoreTake(http->lock, portMAX_DELAY);
    http->stats.bytes += MASKO_HEADER_SIZE + (uint64_t)count * 8;
    http->stats.requests += 2;
    if (header_ms > http->stats.peak_request_ms) http->stats.peak_request_ms = header_ms;
    if (index_ms > http->stats.peak_request_ms) http->stats.peak_request_ms = index_ms;
    xSemaphoreGive(http->lock);
    *out = c;
    ESP_LOGI(TAG, "opened remote pack: %u frames, %u bytes, max JPEG %u",
             (unsigned)count, (unsigned)c->source.header.file_size,
             (unsigned)max_frame);
    return ESP_OK;
}

void masko_http_clip_close(masko_http_clip_t *c)
{
    if (!c) return;
    if (c->http && c->http->lock) {
        xSemaphoreTake(c->http->lock, portMAX_DELAY);
        c->closing = true;
        while (c->jobs_outstanding) {
            xSemaphoreGive(c->http->lock);
            vTaskDelay(pdMS_TO_TICKS(10));
            xSemaphoreTake(c->http->lock, portMAX_DELAY);
        }
        xSemaphoreGive(c->http->lock);
    }
    for (unsigned i = 0; i < 2; i++)
        if (c->slot[i].data) heap_caps_free(c->slot[i].data);
    if (c->offsets) heap_caps_free(c->offsets);
    free(c->url);
    free(c);
}

const masko_frame_source_t *masko_http_clip_source(masko_http_clip_t *c)
{
    return c ? &c->source : NULL;
}

void masko_http_clip_configure(masko_http_clip_t *c, bool loop, uint8_t speed)
{
    if (!c) return;
    if (loop) c->source.header.flags |= MASKO_FLAG_LOOP;
    else c->source.header.flags &= (uint8_t)~MASKO_FLAG_LOOP;
    c->source.frame_step = speed >= 2 ? 2 : 1;
}

masko_http_stats_t masko_http_stats(const masko_http_t *h)
{
    masko_http_stats_t zero = {0};
    if (!h) return zero;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    masko_http_stats_t s = h->stats;
    xSemaphoreGive(h->lock);
    return s;
}

void masko_http_free(masko_http_t *h)
{
    if (!h) return;
    h->running = false;
    xSemaphoreTake(h->worker_done, portMAX_DELAY);
    vQueueDelete(h->jobs);
    vSemaphoreDelete(h->lock);
    vSemaphoreDelete(h->worker_done);
    free(h);
}

esp_err_t masko_http_probe(const char *url)
{
    if (!url) return ESP_ERR_INVALID_ARG;
    transfer_t t = {0};
    esp_http_client_handle_t client = client_new(url, &t);
    if (!client) return ESP_ERR_NO_MEM;
    uint8_t *buf = heap_caps_malloc(256 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { esp_http_client_cleanup(client); return ESP_ERR_NO_MEM; }
    uint32_t total = 0;
    const int64_t a = esp_timer_get_time();
    esp_err_t err = read_range(client, &t, url, 0, 64, buf, &total);
    const int64_t b = esp_timer_get_time();
    if (err == ESP_OK && total >= 256 * 1024)
        err = read_range(client, &t, url, 0, 256 * 1024, buf, &total);
    const int64_t c = esp_timer_get_time();
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Wi-Fi probe: 64B latency=%lld ms, 256KiB=%lld ms, %.1f KiB/s",
                 (long long)((b-a)/1000), (long long)((c-b)/1000),
                 (double)(256*1024) / ((double)(c-b)/1e6) / 1024);
    heap_caps_free(buf);
    esp_http_client_cleanup(client);
    return err;
}
