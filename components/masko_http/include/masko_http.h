#ifndef MASKO_HTTP_H
#define MASKO_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "masko_player.h"

typedef struct masko_http_t masko_http_t;
typedef struct masko_http_clip_t masko_http_clip_t;

typedef struct {
    uint64_t bytes;
    uint64_t frame_bytes;
    uint64_t cache_bytes;
    uint64_t request_us_total;
    uint64_t queue_wait_us_total;
    uint32_t requests;
    uint32_t frame_requests;
    uint32_t cache_requests;
    uint32_t failures;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t peak_request_ms;
    uint32_t delayed_jobs_100ms;
    uint32_t delayed_jobs_500ms;
    uint32_t slow_requests_250ms;
    uint32_t slow_requests_1s;
    uint32_t slow_requests_3s;
} masko_http_stats_t;

typedef struct {
    bool attempted;
    bool filling;
    bool ready;
    size_t downloaded_bytes;
    size_t total_bytes;
} masko_http_cache_stats_t;

/** One worker services frame HTTP range requests. cache_bytes is the size of
 * each of two frame-aligned PSRAM cache slots per open clip. */
esp_err_t masko_http_new(size_t cache_bytes, masko_http_t **out);
void masko_http_free(masko_http_t *http);

/** Fetch and validate a pack header and index. Does not fetch its frames. */
esp_err_t masko_http_clip_open(masko_http_t *http, const char *url,
                               masko_http_clip_t **out);
void masko_http_clip_close(masko_http_clip_t *clip);
const masko_frame_source_t *masko_http_clip_source(masko_http_clip_t *clip);
/** Apply the delivery graph's loop and speed metadata to an opened pack. */
void masko_http_clip_configure(masko_http_clip_t *clip, bool loop, uint8_t speed);
/** Start a background download of this clip's frame data into PSRAM. Once
 * complete, frames are served from memory and the two range slots are freed.
 * Assets larger than max_bytes or memory-limited assets keep streaming. */
esp_err_t masko_http_clip_cache_start(masko_http_clip_t *clip, size_t max_bytes);
/** Prefetch-only variant: releases the unused rolling frame slots so a large
 * contiguous PSRAM block is available for the complete clip. Do not attach
 * this clip to a player until the cache reports ready. */
esp_err_t masko_http_clip_cache_prefetch(masko_http_clip_t *clip, size_t max_bytes);
masko_http_stats_t masko_http_stats(const masko_http_t *http);
masko_http_cache_stats_t masko_http_clip_cache_stats(const masko_http_clip_t *clip);

/** A board-side network probe using the same TLS and Range path as playback.
 * Downloads 64 bytes, then a 256 KiB sample and logs timings and throughput. */
esp_err_t masko_http_probe(const char *url);

#endif
