#ifndef MASKO_HTTP_H
#define MASKO_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "masko_player.h"

typedef struct masko_http_t masko_http_t;
typedef struct masko_http_clip_t masko_http_clip_t;

typedef struct {
    uint64_t bytes;
    uint32_t requests;
    uint32_t failures;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t peak_request_ms;
} masko_http_stats_t;

/** One worker services all HTTP range requests. cache_bytes is the size of
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
masko_http_stats_t masko_http_stats(const masko_http_t *http);

/** A board-side network probe using the same TLS and Range path as playback.
 * Downloads 64 bytes, then a 256 KiB sample and logs timings and throughput. */
esp_err_t masko_http_probe(const char *url);

#endif
