/* Full Kylo delivery graph over HTTP range requests. The manifest is compiled
 * into this example; animation packs are never copied into the app image. */
#include "sdkconfig.h"

#if CONFIG_MASKO_STREAM_ENABLE

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "masko_calib.h"
#include "masko_display.h"
#include "masko_graph_json.h"
#include "masko_http.h"
#include "masko_player.h"

static const char *TAG = "kylo_stream";
/* Streaming keeps TLS and Wi-Fi buffers alive while the panel is active.
 * A 50-row PSRAM-backed push needs a 46,600-byte internal DMA bounce buffer,
 * which can fail after the network heap fragments. */
#define STREAM_BAND_ROWS 8
#define STREAM_BAND_BYTES (MASKO_PANEL_W * STREAM_BAND_ROWS * 2)
extern const char manifest_start[] asm("_binary_kylo_stream_mascot_json_start");
extern const char manifest_end[] asm("_binary_kylo_stream_mascot_json_end");

typedef enum { MSG_TOUCH, MSG_PLAYER } msg_kind_t;
typedef struct { msg_kind_t kind; int event; int64_t at_us; } msg_t;

typedef struct {
    masko_http_t *http;
    masko_player_t *player;
    masko_graph_json_t parsed;
    masko_graph_t graph;
    QueueHandle_t events;
    esp_lcd_touch_handle_t touch;
    masko_http_clip_t *active_clip, *pending_clip;
    masko_http_clip_t *prefetch_clip;
    masko_http_clip_t *prefetch_transition_clip;
    const masko_edge_t *active_edge, *pending_edge;
    const masko_edge_t *prefetch_edge;
    const masko_edge_t *prefetch_transition_edge;
    const masko_edge_t *prefetch_transition_failed_edge;
    int current_node, wanted_node, next_touch_node;
    int prefetch_node, prefetch_failed_node;
    int64_t request_us, edge_request_us, loop_start_us;
    int64_t stats_start_us;
    int64_t diag_start_us;
    int64_t last_prepare_attempt_us;
    bool need_next;
    masko_stats_t last_player_stats;
    masko_http_stats_t last_http_stats;
    masko_stats_t diag_player_stats;
    masko_http_stats_t diag_http_stats;
} stream_app_t;

static stream_app_t app;

static void wifi_event(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    EventGroupHandle_t flags = ctx;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", (unsigned)event->reason);
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(flags, BIT0);
}

static esp_err_t wifi_start(void)
{
    if (!CONFIG_MASKO_STREAM_SSID[0]) {
        ESP_LOGE(TAG, "Set Masko demo > Wi-Fi SSID and password in menuconfig");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    EventGroupHandle_t flags = xEventGroupCreate();
    if (!flags) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event, flags));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event, flags));
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_MASKO_STREAM_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_MASKO_STREAM_PASSWORD,
            sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#if CONFIG_MASKO_STREAM_PROBE_ONLY
    wifi_scan_config_t scan = {
        .ssid = (uint8_t *)CONFIG_MASKO_STREAM_SSID,
        .show_hidden = true,
    };
    err = esp_wifi_scan_start(&scan, true);
    if (err == ESP_OK) {
        uint16_t found = 0;
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&found));
        ESP_LOGI(TAG, "Wi-Fi scan: %u matching access points", (unsigned)found);
    } else {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    }
#endif
    ESP_ERROR_CHECK(esp_wifi_connect());
    const EventBits_t bits = xEventGroupWaitBits(flags, BIT0, pdFALSE, pdTRUE,
                                                 pdMS_TO_TICKS(30000));
    if (!(bits & BIT0)) {
        ESP_LOGE(TAG, "Wi-Fi connection timed out after 30 seconds");
        return ESP_ERR_TIMEOUT;
    }
    wifi_ap_record_t ap = {0};
    wifi_bandwidth_t bandwidth = WIFI_BW20;
    const esp_err_t bw_err = esp_wifi_get_bandwidth(WIFI_IF_STA, &bandwidth);
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        ESP_LOGI(TAG, "Wi-Fi connected; RSSI %d dBm, channel %u, BW%s",
                 ap.rssi, (unsigned)ap.primary,
                 bw_err == ESP_OK ? (bandwidth == WIFI_BW40 ? "40" : "20") : "?");
    ESP_LOGI(TAG, "heap after Wi-Fi: internal=%u, DMA_largest=%u, PSRAM=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

static void player_event(masko_event_t ev, void *ctx)
{
    stream_app_t *a = ctx;
    const msg_t m = { MSG_PLAYER, (int)ev, esp_timer_get_time() };
    xQueueSend(a->events, &m, 0);
}

static int node_index(stream_app_t *a, const char *id)
{
    return masko_graph_find_node(&a->graph, id);
}

static int edge_index(stream_app_t *a, const masko_edge_t *edge)
{
    return edge ? (int)(edge-a->graph.edges) : -1;
}

static bool edge_reverse(stream_app_t *a, const masko_edge_t *edge)
{
    const int i = edge_index(a, edge);
    return i >= 0 && i < a->parsed.nedges && a->parsed.edge_reverse[i];
}

static uint8_t edge_speed(stream_app_t *a, const masko_edge_t *edge)
{
    const int i = edge_index(a, edge);
    return i >= 0 && i < a->parsed.nedges ? a->parsed.edge_speed[i] : 1;
}

static const masko_edge_t *direct_edge(stream_app_t *a, int src, int dst)
{
    if (src < 0 || dst < 0) return NULL;
    const char *s = a->graph.nodes[src].id, *d = a->graph.nodes[dst].id;
    for (int i = 0; i < a->graph.nedges; i++) {
        const masko_edge_t *e = &a->graph.edges[i];
        if (!e->is_loop && strcmp(e->source, s) == 0 &&
            strcmp(e->target, d) == 0 && (e->clip || edge_reverse(a, e))) return e;
    }
    return NULL;
}

static const char *edge_url(stream_app_t *a, const masko_edge_t *edge,
                            bool *reverse)
{
    *reverse = false;
    if (edge->clip) return edge->clip;
    if (!edge_reverse(a, edge)) return NULL;
    const int rest = node_index(a, "jar-gazing");
    const int from = node_index(a, edge->source);
    const masko_edge_t *forward = direct_edge(a, rest, from);
    if (!forward) return NULL;
    *reverse = true;
    return forward->clip;
}

static const masko_edge_t *next_edge(stream_app_t *a)
{
    if (a->current_node == a->wanted_node)
        return masko_graph_loop(&a->graph, a->current_node);
    const masko_edge_t *edge = direct_edge(a, a->current_node, a->wanted_node);
    if (edge) return edge;
    const int rest = node_index(a, "jar-gazing");
    if (a->current_node != rest)
        return direct_edge(a, a->current_node, rest);
    return masko_graph_loop(&a->graph, a->wanted_node);
}

static void discard_prefetch(stream_app_t *a)
{
    if (a->prefetch_clip) masko_http_clip_close(a->prefetch_clip);
    a->prefetch_clip = NULL;
    a->prefetch_edge = NULL;
    a->prefetch_node = -1;
}

static void discard_prefetch_transition(stream_app_t *a)
{
    if (a->prefetch_transition_clip)
        masko_http_clip_close(a->prefetch_transition_clip);
    a->prefetch_transition_clip = NULL;
    a->prefetch_transition_edge = NULL;
}

static const masko_edge_t *first_transition_toward(stream_app_t *a, int target)
{
    if (a->current_node < 0 || target < 0 || a->current_node == target)
        return NULL;
    const masko_edge_t *edge = direct_edge(a, a->current_node, target);
    if (edge) return edge;
    const int rest = node_index(a, "jar-gazing");
    return a->current_node == rest ? NULL :
        direct_edge(a, a->current_node, rest);
}

static void prefetch_next_transition(stream_app_t *a)
{
    if (a->pending_clip || !a->active_edge || !a->active_edge->is_loop ||
        a->current_node < 0 || a->next_touch_node < 0) return;
    const masko_edge_t *edge = first_transition_toward(a, a->next_touch_node);
    if (a->prefetch_transition_clip) {
        const masko_http_cache_stats_t cache =
            masko_http_clip_cache_stats(a->prefetch_transition_clip);
        if (!cache.ready && !cache.filling) {
            ESP_LOGW(TAG, "transition prefetch failed for %s -> %s",
                     a->prefetch_transition_edge->source,
                     a->prefetch_transition_edge->target);
            a->prefetch_transition_failed_edge = a->prefetch_transition_edge;
            discard_prefetch_transition(a);
        } else if (a->prefetch_transition_edge == edge) {
            return;
        }
        discard_prefetch_transition(a);
    }
    if (!edge || edge == a->prefetch_transition_failed_edge) return;
    if (!masko_http_clip_cache_stats(a->active_clip).ready) return;

    bool reverse = false;
    const char *url = edge_url(a, edge, &reverse);
    if (!url) { a->prefetch_transition_failed_edge = edge; return; }
    masko_http_clip_t *clip = NULL;
    esp_err_t err = masko_http_clip_open(a->http, url, &clip);
    if (err == ESP_OK) {
        masko_http_clip_configure(clip, false, edge_speed(a, edge));
        err = masko_http_clip_cache_prefetch(
            clip, CONFIG_MASKO_STREAM_FULL_CACHE_KIB*1024u);
    }
    if (err != ESP_OK) {
        if (clip) masko_http_clip_close(clip);
        ESP_LOGW(TAG, "transition prefetch unavailable for %s -> %s: %s",
                 edge->source, edge->target, esp_err_to_name(err));
        a->prefetch_transition_failed_edge = edge;
        return;
    }
    a->prefetch_transition_failed_edge = NULL;
    a->prefetch_transition_clip = clip;
    a->prefetch_transition_edge = edge;
    ESP_LOGI(TAG, "prefetching next touch transition: %s -> %s, reverse=%d",
             edge->source, edge->target, reverse);
}

static void prefetch_next_loop(stream_app_t *a)
{
    if (a->pending_clip || !a->active_edge || !a->active_edge->is_loop ||
        a->current_node < 0 || a->next_touch_node < 0 ||
        a->next_touch_node == a->current_node)
        return;

    if (a->prefetch_clip) {
        if (a->prefetch_node == a->next_touch_node ||
            a->prefetch_node == a->wanted_node)
            return;
        discard_prefetch(a);
    }

    /* Keep playback on its in-memory cache while warming the next touch target. */
    const masko_http_cache_stats_t active_cache =
        masko_http_clip_cache_stats(a->active_clip);
    if (!active_cache.ready) return;
    if (a->prefetch_transition_clip &&
        !masko_http_clip_cache_stats(a->prefetch_transition_clip).ready)
        return;

    const int node = a->next_touch_node;
    if (a->prefetch_failed_node == node) return;
    const masko_edge_t *edge = masko_graph_loop(&a->graph, node);
    if (!edge || !edge->clip) {
        a->prefetch_failed_node = node;
        return;
    }

    masko_http_clip_t *clip = NULL;
    esp_err_t err = masko_http_clip_open(a->http, edge->clip, &clip);
    if (err == ESP_OK) {
        masko_http_clip_configure(clip, true, edge_speed(a, edge));
        err = masko_http_clip_cache_prefetch(
            clip, CONFIG_MASKO_STREAM_FULL_CACHE_KIB*1024u);
    }
    if (err != ESP_OK) {
        if (clip) masko_http_clip_close(clip);
        ESP_LOGW(TAG, "next-loop prefetch unavailable for %s: %s", edge->target,
                 esp_err_to_name(err));
        a->prefetch_failed_node = node;
        return;
    }
    a->prefetch_failed_node = -1;
    a->prefetch_clip = clip;
    a->prefetch_edge = edge;
    a->prefetch_node = node;
    ESP_LOGI(TAG, "prefetching next touch loop: %s", edge->target);
}

static esp_err_t queue_edge(stream_app_t *a, const masko_edge_t *edge,
                            bool immediate)
{
    if (!edge || a->pending_clip) return ESP_ERR_INVALID_STATE;
    a->last_prepare_attempt_us = esp_timer_get_time();
    bool reverse = false;
    const char *url = edge_url(a, edge, &reverse);
    if (!url) {
        ESP_LOGE(TAG, "edge has no playable pack: %s -> %s", edge->source,
                 edge->target);
        return ESP_ERR_NOT_FOUND;
    }
    const int64_t start = esp_timer_get_time();
    masko_http_clip_t *clip = NULL;
    const int target_node = edge->is_loop ? node_index(a, edge->target) : -1;
    bool prefetched = false;
    if (!edge->is_loop && a->prefetch_transition_clip) {
        if (edge == a->prefetch_transition_edge &&
            masko_http_clip_cache_stats(a->prefetch_transition_clip).ready) {
            clip = a->prefetch_transition_clip;
            a->prefetch_transition_clip = NULL;
            a->prefetch_transition_edge = NULL;
            prefetched = true;
            ESP_LOGI(TAG, "using prefetched transition: %s -> %s",
                     edge->source, edge->target);
        } else {
            discard_prefetch_transition(a);
        }
    }
    if (edge->is_loop && a->prefetch_clip && target_node == a->prefetch_node) {
        if (masko_http_clip_cache_stats(a->prefetch_clip).ready) {
            clip = a->prefetch_clip;
            a->prefetch_clip = NULL;
            ESP_LOGI(TAG, "using prefetched loop: %s", edge->target);
            discard_prefetch(a);
            prefetched = true;
        } else {
            ESP_LOGW(TAG, "target loop prefetch not ready; using live range stream");
            discard_prefetch(a);
        }
    }
    if (!prefetched) {
        esp_err_t open_err = masko_http_clip_open(a->http, url, &clip);
        if (open_err != ESP_OK) {
            ESP_LOGE(TAG, "cannot open %s: %s", url, esp_err_to_name(open_err));
            return open_err;
        }
    }
    masko_http_clip_configure(clip, edge->is_loop, edge_speed(a, edge));
    esp_err_t err;
    err = masko_player_set_source(a->player, masko_http_clip_source(clip),
                                  reverse, immediate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot prepare %s: %s", url, esp_err_to_name(err));
        masko_http_clip_close(clip);
        return err;
    }
    a->pending_clip = clip;
    a->pending_edge = edge;
    a->need_next = false;
    a->edge_request_us = start;
    ESP_LOGI(TAG, "prepared %s -> %s (%s) in %lld ms; reverse=%d",
             edge->source, edge->target, edge->is_loop ? "loop" : "transition",
             (long long)((esp_timer_get_time()-start)/1000), reverse);
    return ESP_OK;
}

static void report(stream_app_t *a)
{
    const masko_stats_t p = masko_player_stats(a->player);
    const masko_http_stats_t h = masko_http_stats(a->http);
    const double sec = (double)(esp_timer_get_time()-a->stats_start_us)/1e6;
    const uint32_t shown = p.frames_shown-a->last_player_stats.frames_shown;
    const int64_t decode_total = p.decode_us_total-a->last_player_stats.decode_us_total;
    const int64_t push_total = p.push_us_total-a->last_player_stats.push_us_total;
    const int64_t decode_avg = shown ? decode_total/shown : 0;
    const int64_t push_avg = shown ? push_total/shown : 0;
    const int64_t slower_stage = decode_avg > push_avg ? decode_avg : push_avg;
    const double render_ceiling = slower_stage > 0 ?
        1e6/(double)slower_stage : 0;
    ESP_LOGI(TAG, "30s: %.1f fps, shown=%u, dropped=%u, underrun=%u, "
                  "stream stalls=%u, wait=%lld ms, HTTP bytes=%llu, "
                  "requests=%u, failures=%u, cache hits=%u, misses=%u, "
                  "peak request=%u ms, decode=%lld/%lld us avg/max, "
                  "push=%lld/%lld us avg/max, render ceiling=%.1f fps, "
                  "heap internal=%u (low %u) DMA_largest=%u PSRAM=%u (low %u)",
             sec > 0 ? (p.frames_shown-a->last_player_stats.frames_shown)/sec : 0,
             (unsigned)shown,
             (unsigned)(p.frames_dropped-a->last_player_stats.frames_dropped),
             (unsigned)(p.underruns-a->last_player_stats.underruns),
             (unsigned)(p.stream_stalls-a->last_player_stats.stream_stalls),
             (long long)((p.stream_wait_us-a->last_player_stats.stream_wait_us)/1000),
             (unsigned long long)(h.bytes-a->last_http_stats.bytes),
             (unsigned)(h.requests-a->last_http_stats.requests),
             (unsigned)(h.failures-a->last_http_stats.failures),
             (unsigned)(h.cache_hits-a->last_http_stats.cache_hits),
             (unsigned)(h.cache_misses-a->last_http_stats.cache_misses),
             (unsigned)h.peak_request_ms,
             (long long)decode_avg, (long long)p.decode_us_max,
             (long long)push_avg, (long long)p.push_us_max, render_ceiling,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    a->last_player_stats = p;
    a->last_http_stats = h;
    a->stats_start_us = esp_timer_get_time();
}

static void report_diagnostic(stream_app_t *a)
{
    const masko_stats_t p = masko_player_stats(a->player);
    const masko_http_stats_t h = masko_http_stats(a->http);
    const masko_http_cache_stats_t c = masko_http_clip_cache_stats(a->active_clip);
    const double sec = (double)(esp_timer_get_time()-a->diag_start_us)/1e6;
    const uint32_t shown = p.frames_shown-a->diag_player_stats.frames_shown;
    const uint32_t frame_reqs = h.frame_requests-a->diag_http_stats.frame_requests;
    const uint32_t cache_reqs = h.cache_requests-a->diag_http_stats.cache_requests;
    const uint64_t request_us = h.request_us_total-a->diag_http_stats.request_us_total;
    const uint64_t queue_us = h.queue_wait_us_total-a->diag_http_stats.queue_wait_us_total;
    const char *cache_state = c.ready ? "ready" : c.filling ? "fill" :
                              c.attempted ? "failed" : "off";
    const char *source = a->active_edge ? a->active_edge->source : "?";
    const char *target = a->active_edge ? a->active_edge->target : "?";

    ESP_LOGI(TAG, "diag state=%s->%s cache=%s %u/%uB fps=%.1f frames=%u "
                  "src_reads=%u src_wait=%lldms src_max=%lldms stalls=%u "
                  "queue_underrun=%u display_gap_max=%lldms gaps>100/500/1000ms=%u/%u/%u",
             source, target, cache_state, (unsigned)c.downloaded_bytes,
             (unsigned)c.total_bytes, sec > 0 ? shown/sec : 0.0, (unsigned)shown,
             (unsigned)(p.source_reads-a->diag_player_stats.source_reads),
             (long long)((p.stream_wait_us-a->diag_player_stats.stream_wait_us)/1000),
             (long long)(p.stream_wait_us_max/1000),
             (unsigned)(p.stream_stalls-a->diag_player_stats.stream_stalls),
             (unsigned)(p.underruns-a->diag_player_stats.underruns),
             (long long)(p.display_gap_us_max/1000),
             (unsigned)(p.display_gaps_100ms-a->diag_player_stats.display_gaps_100ms),
             (unsigned)(p.display_gaps_500ms-a->diag_player_stats.display_gaps_500ms),
             (unsigned)(p.display_gaps_1s-a->diag_player_stats.display_gaps_1s));
    ESP_LOGI(TAG, "diag_net req_frame/cache=%u/%u bytes_frame/cache=%llu/%llu "
                  "req_avg=%llums queue_avg=%llums delayed>100/500ms=%u/%u "
                  "slow>250/1000/3000ms=%u/%u/%u decoder=%lldus push=%lldus "
                  "push_qwait=%lldms max=%lldms heap_int=%u DMA_largest=%u PSRAM=%u",
             (unsigned)frame_reqs, (unsigned)cache_reqs,
             (unsigned long long)(h.frame_bytes-a->diag_http_stats.frame_bytes),
             (unsigned long long)(h.cache_bytes-a->diag_http_stats.cache_bytes),
             (unsigned long long)(h.requests ? request_us/((frame_reqs+cache_reqs) ?
                 (frame_reqs+cache_reqs) : 1)/1000 : 0),
             (unsigned long long)(h.requests ? queue_us/((frame_reqs+cache_reqs) ?
                 (frame_reqs+cache_reqs) : 1)/1000 : 0),
             (unsigned)(h.delayed_jobs_100ms-a->diag_http_stats.delayed_jobs_100ms),
             (unsigned)(h.delayed_jobs_500ms-a->diag_http_stats.delayed_jobs_500ms),
             (unsigned)(h.slow_requests_250ms-a->diag_http_stats.slow_requests_250ms),
             (unsigned)(h.slow_requests_1s-a->diag_http_stats.slow_requests_1s),
             (unsigned)(h.slow_requests_3s-a->diag_http_stats.slow_requests_3s),
             (long long)(shown ? (p.decode_us_total-a->diag_player_stats.decode_us_total)/shown : 0),
             (long long)(shown ? (p.push_us_total-a->diag_player_stats.push_us_total)/shown : 0),
             (long long)((p.push_queue_wait_us_total-a->diag_player_stats.push_queue_wait_us_total)/1000),
             (long long)(p.push_queue_wait_us_max/1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (a->prefetch_clip) {
        const masko_http_cache_stats_t prefetch =
            masko_http_clip_cache_stats(a->prefetch_clip);
        ESP_LOGI(TAG, "diag_prefetch state=%s cache=%s %u/%uB",
                 a->prefetch_edge ? a->prefetch_edge->target : "?",
                 prefetch.ready ? "ready" : prefetch.filling ? "fill" : "failed",
                 (unsigned)prefetch.downloaded_bytes, (unsigned)prefetch.total_bytes);
    }
    if (a->prefetch_transition_clip) {
        const masko_http_cache_stats_t transition =
            masko_http_clip_cache_stats(a->prefetch_transition_clip);
        ESP_LOGI(TAG, "diag_transition_prefetch state=%s->%s cache=%s %u/%uB",
                 a->prefetch_transition_edge->source,
                 a->prefetch_transition_edge->target,
                 transition.ready ? "ready" : transition.filling ? "fill" : "failed",
                 (unsigned)transition.downloaded_bytes,
                 (unsigned)transition.total_bytes);
    }
    a->diag_player_stats = p;
    a->diag_http_stats = h;
    a->diag_start_us = esp_timer_get_time();
}

static void start_active_cache(stream_app_t *a)
{
#if CONFIG_MASKO_STREAM_FULL_CACHE_KIB > 0
    /* One-shot transitions are short-lived; caching them duplicates traffic
     * and the cache is freed as soon as the target loop starts. */
    if (!a->active_clip || !a->active_edge || !a->active_edge->is_loop) return;
    if (masko_http_clip_cache_stats(a->active_clip).attempted) return;
    const esp_err_t err = masko_http_clip_cache_start(
        a->active_clip, CONFIG_MASKO_STREAM_FULL_CACHE_KIB*1024u);
    if (err != ESP_OK)
        ESP_LOGI(TAG, "full clip cache unavailable; keep range streaming: %s",
                 esp_err_to_name(err));
#else
    (void)a;
#endif
}

static void input_task(void *ctx)
{
    stream_app_t *a = ctx;
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&button);
    bool was_down = false;
    while (true) {
        esp_lcd_touch_point_data_t point[1];
        uint8_t count = 0;
        const bool touch = esp_lcd_touch_read_data(a->touch) == ESP_OK &&
            esp_lcd_touch_get_data(a->touch, point, &count, 1) == ESP_OK && count;
        const bool down = touch || gpio_get_level(GPIO_NUM_0) == 0;
        if (down && !was_down) {
            const msg_t m = { MSG_TOUCH, touch ? 1 : 0, esp_timer_get_time() };
            xQueueSend(a->events, &m, 0);
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void handle_message(stream_app_t *a, msg_t m)
{
    if (m.kind == MSG_TOUCH) {
        a->wanted_node = a->next_touch_node;
        a->next_touch_node = (a->next_touch_node+1) % a->graph.nnodes;
        if (a->prefetch_clip && a->prefetch_node != a->wanted_node)
            discard_prefetch(a);
        if (a->prefetch_transition_clip &&
            a->prefetch_transition_edge != first_transition_toward(a, a->wanted_node))
            discard_prefetch_transition(a);
        a->request_us = m.at_us;
        ESP_LOGI(TAG, "%s requested %s", m.event ? "touch" : "BOOT",
                 a->graph.nodes[a->wanted_node].id);
        if (a->active_edge && a->active_edge->is_loop && !a->pending_clip &&
            a->current_node != a->wanted_node)
            a->need_next = queue_edge(a, next_edge(a), true) != ESP_OK;
        return;
    }
    switch ((masko_event_t)m.event) {
    case MASKO_EV_CLIP_START: {
        if (a->pending_clip) {
            masko_http_clip_t *old = a->active_clip;
            a->active_clip = a->pending_clip;
            a->active_edge = a->pending_edge;
            a->pending_clip = NULL;
            a->pending_edge = NULL;
            if (old) masko_http_clip_close(old);
            start_active_cache(a);
        }
        if (a->active_edge && a->active_edge->is_loop)
            a->loop_start_us = m.at_us;
        if (a->active_edge && a->active_edge->is_loop)
            a->prefetch_transition_failed_edge = NULL;
        ESP_LOGI(TAG, "clip started: %s -> %s", a->active_edge->source,
                 a->active_edge->target);
        break;
    }
    case MASKO_EV_FIRST_FRAME_SHOWN:
        if (a->active_edge) {
            ESP_LOGI(TAG, "first pixel: %s -> %s, preparation-to-display=%lld ms, "
                          "input-to-display=%lld ms",
                     a->active_edge->source, a->active_edge->target,
                     (long long)((m.at_us-a->edge_request_us)/1000),
                     a->request_us ? (long long)((m.at_us-a->request_us)/1000) : -1);
        }
        break;
    case MASKO_EV_CLIP_END:
        if (a->active_edge && !a->active_edge->is_loop) {
            a->current_node = node_index(a, a->active_edge->target);
            if (a->current_node < 0) break;
            if (!a->pending_clip)
                a->need_next = queue_edge(a, next_edge(a), true) != ESP_OK;
        }
        break;
    case MASKO_EV_UNDERRUN: break;
    case MASKO_EV_ERROR: ESP_LOGE(TAG, "player error"); break;
    }
}

void masko_stream_demo_main(void)
{
    memset(&app, 0, sizeof(app));
    const size_t manifest_len = (size_t)(manifest_end-manifest_start);
    if (masko_graph_json_parse(manifest_start, manifest_len, &app.parsed) != MASKO_GRAPH_OK ||
        masko_graph_json_bind(&app.parsed, &app.graph) != MASKO_GRAPH_OK) {
        ESP_LOGE(TAG, "bundled Kylo manifest is invalid");
        return;
    }
    app.current_node = node_index(&app, app.graph.initial_node);
    app.wanted_node = app.current_node;
    app.next_touch_node = (app.current_node+1) % app.graph.nnodes;
    app.prefetch_node = -1;
    app.prefetch_failed_node = -1;
    const masko_edge_t *initial = masko_graph_loop(&app.graph, app.current_node);
    if (!initial || !initial->clip) { ESP_LOGE(TAG, "initial loop missing"); return; }
    ESP_LOGI(TAG, "Kylo manifest: %d states, %d edges", app.graph.nnodes,
             app.graph.nedges);
    if (wifi_start() != ESP_OK) return;
    if (masko_http_probe(initial->clip) != ESP_OK) {
        ESP_LOGE(TAG, "CDN range probe failed; check Wi-Fi before streaming");
        return;
    }
#if CONFIG_MASKO_STREAM_PROBE_ONLY
    {
        ESP_LOGI(TAG, "probe-only mode complete");
        return;
    }
#endif
    ESP_ERROR_CHECK(masko_http_new(CONFIG_MASKO_STREAM_CACHE_KIB*1024u, &app.http));
    ESP_ERROR_CHECK(masko_http_clip_open(app.http, initial->clip, &app.active_clip));
    masko_http_clip_configure(app.active_clip, true, 1);
    const masko_frame_source_t *first_source = masko_http_clip_source(app.active_clip);
    uint8_t *warm = heap_caps_malloc(first_source->max_frame_bytes,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t warm_len = 0;
    const esp_err_t warm_err = warm ?
        first_source->read_frame(first_source->context, 0, false, warm,
                                 first_source->max_frame_bytes, &warm_len,
                                 8000) : ESP_ERR_NO_MEM;
    if (warm_err != ESP_OK) {
        ESP_LOGE(TAG, "first streamed frame unavailable: %s",
                 esp_err_to_name(warm_err));
        if (warm) heap_caps_free(warm);
        masko_http_clip_close(app.active_clip);
        masko_http_free(app.http);
        return;
    }
    heap_caps_free(warm);
    app.active_edge = initial;
    const bsp_display_config_t disp_cfg = { .max_transfer_sz = STREAM_BAND_BYTES };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(bsp_display_new(&disp_cfg, &panel, &io));
    ESP_ERROR_CHECK(bsp_display_brightness_init());
    bsp_display_brightness_set(80);
    const bsp_display_cfg_t touch_cfg = {0};
    ESP_ERROR_CHECK(bsp_touch_new(&touch_cfg, &app.touch));
    masko_display_cfg_t dc = {
        .panel = panel, .canvas_w = BSP_LCD_H_RES,
        .canvas_h = BSP_LCD_V_RES, .band_rows = STREAM_BAND_ROWS,
    };
    masko_display_t *display = NULL;
    ESP_ERROR_CHECK(masko_display_new(&dc, &display));
    masko_display_fill(display, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES,
                       masko_http_clip_source(app.active_clip)->header.bg_rgb565);
    app.events = xQueueCreate(32, sizeof(msg_t));
    if (!app.events) { ESP_LOGE(TAG, "event queue allocation failed"); return; }
    const masko_player_cfg_t pc = {
        .frame_source = masko_http_clip_source(app.active_clip),
        .display = display, .decode_core = 1, .max_fps = 24,
        .stats_enabled = true, .on_event = player_event, .user = &app,
    };
    ESP_ERROR_CHECK(masko_player_create(&pc, &app.player));
    app.edge_request_us = esp_timer_get_time();
    ESP_ERROR_CHECK(masko_player_play(app.player));
    start_active_cache(&app);
    if (xTaskCreatePinnedToCore(input_task, "masko_input", 4096, &app, 3,
                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "input task allocation failed"); return;
    }
    app.stats_start_us = esp_timer_get_time();
    app.diag_start_us = app.stats_start_us;
    ESP_LOGI(TAG, "streaming; touch/BOOT cycles all %d Kylo states", app.graph.nnodes);
    while (true) {
        msg_t m;
        if (xQueueReceive(app.events, &m, pdMS_TO_TICKS(100)) == pdTRUE)
            handle_message(&app, m);
        if (!app.pending_clip &&
            (app.need_next || (app.active_edge && app.active_edge->is_loop &&
                               app.current_node != app.wanted_node)) &&
            esp_timer_get_time()-app.last_prepare_attempt_us > 2000000)
            app.need_next = queue_edge(&app, next_edge(&app), true) != ESP_OK;
        prefetch_next_transition(&app);
        prefetch_next_loop(&app);
        if (esp_timer_get_time()-app.stats_start_us >= 30000000)
            report(&app);
        if (esp_timer_get_time()-app.diag_start_us >= 1000000)
            report_diagnostic(&app);
    }
}

#endif
