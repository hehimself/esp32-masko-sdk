/**
 * @file masko_player.h
 * @brief Packed-clip playback with decode and panel push overlapped.
 *
 * Decode runs on core 1 and pushes run on core 0. They contend for very
 * different resources -- one is CPU-bound in esp_jpeg, the other is a SPI DMA
 * transfer -- so overlapping them costs far less than running them in series.
 * Measured at 466x466 serial decode+push was 17.9 fps; overlapped it clears the
 * 41.7 ms budget needed for 24 fps.
 */
#ifndef MASKO_PLAYER_H
#define MASKO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "masko_pack.h"
#include "masko_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct masko_player_t masko_player_t;

typedef enum {
    MASKO_EV_CLIP_START,
    MASKO_EV_CLIP_END,
    MASKO_EV_UNDERRUN,
    MASKO_EV_ERROR,
    MASKO_EV_FIRST_FRAME_SHOWN, /* emitted after the first frame reaches the panel */
} masko_event_t;

typedef void (*masko_event_cb_t)(masko_event_t ev, void *user);

/** A pack whose compressed frames are supplied on demand (for example over HTTP).
 * The caller owns this object and keeps it alive until the player has switched
 * away from it. read_frame copies one complete JPEG into dst. A temporary
 * network miss returns ESP_ERR_TIMEOUT; the player holds the last picture and
 * retries the same frame rather than skipping it. */
typedef struct {
    masko_pack_header_t header;
    size_t max_frame_bytes;
    uint8_t frame_step; /* 1 normally; 2 shows a speed=2 transition at 24 fps */
    void *context;
    esp_err_t (*read_frame)(void *context, uint32_t frame, bool reverse,
                            uint8_t *dst, size_t capacity, uint32_t *length,
                            uint32_t timeout_ms);
} masko_frame_source_t;

typedef struct {
    const uint8_t     *pack_data;
    size_t             pack_size;
    const masko_frame_source_t *frame_source; /* optional alternative to pack_data */
    masko_display_t   *display;
    uint8_t            max_fps;       /* cap; 0 = use the pack's own fps */
    int8_t             decode_core;   /* 0, 1, or -1 for any */
    bool               stats_enabled;
    masko_event_cb_t   on_event;
    void              *user;
} masko_player_cfg_t;

typedef struct {
    uint32_t frames_shown;
    uint32_t frames_dropped;
    uint32_t underruns;
    uint32_t stream_stalls;
    uint32_t source_reads;
    uint32_t display_gaps_100ms;
    uint32_t display_gaps_500ms;
    uint32_t display_gaps_1s;
    int64_t  stream_wait_us;
    int64_t  stream_wait_us_max;
    int64_t  push_queue_wait_us_total;
    int64_t  push_queue_wait_us_max;
    int64_t  display_gap_us_max;
    int64_t  decode_us_avg;
    int64_t  decode_us_max;
    int64_t  push_us_avg;
    int64_t  push_us_max;
    int64_t  decode_us_total;
    int64_t  push_us_total;
    double   achieved_fps;
    double   ceiling_fps;   /* pipeline limit: 1e6 / max(decode, push) */
} masko_stats_t;

esp_err_t masko_player_create(const masko_player_cfg_t *cfg, masko_player_t **out);

/**
 * @brief Prepare a pack and switch at the active clip boundary.
 *
 * This is what makes a multi-state mascot possible: one player, many clips.
 * The candidate's first frame is decoded before it becomes active. If decoding
 * fails, playback stays on the current pack and MASKO_EV_ERROR is reported.
 * Already decoded outgoing frames are presented in order before the candidate.
 *
 * The new pack must have the SAME bbox dimensions as the one the player was
 * created with. The decode buffers and the panel update rectangle are sized
 * from that geometry, so a differently-sized clip would overrun them; this
 * returns ESP_ERR_INVALID_SIZE instead. Bundle your clips with a shared bbox
 * (the host packer does this automatically) and the constraint holds.
 *
 * The candidate's first frame is decoded into a dedicated PSRAM buffer before
 * this call returns. The active clip remains visible until its loop boundary,
 * or until a non-looping clip ends. The prepared frame is presented after all
 * outgoing frames. A non-looping clip holds its final frame and emits
 * MASKO_EV_CLIP_END once, waiting for the controller to queue another pack.
 * If preparation fails, playback stays on the current pack and emits
 * MASKO_EV_ERROR.
 *
 * @param data  Pack bytes. Must stay valid and unmoved while this pack can be
 *              played. Keep the old pack alive until it is no longer active.
 * @param size  Length of the pack.
 */
esp_err_t masko_player_set_pack(masko_player_t *p, const uint8_t *data, size_t size);

/**
 * @brief Prepare a pack and play it forwards or backwards at the clip boundary.
 *
 * Reverse playback uses the same validated pack and starts at its final frame;
 * it does not allocate a reversed copy. This is useful for free authored
 * reverse transitions that reuse the forward animation.
 */
esp_err_t masko_player_set_pack_direction(masko_player_t *p, const uint8_t *data,
                                          size_t size, bool reverse);

/**
 * @brief Prepare a pack and switch to it on the next decoded frame.
 *
 * Use this for responsive reactions where waiting for a long loop boundary
 * would be undesirable. The first frame is validated and decoded before the
 * active clip is replaced, so the display never clears while preparing.
 */
esp_err_t masko_player_set_pack_direction_immediate(masko_player_t *p,
                                                    const uint8_t *data,
                                                    size_t size, bool reverse);

/** Prepare a frame source without downloading a complete pack. The caller may
 * invoke this from a controller task; it can wait for the candidate's first
 * compressed frame. Never call it from a player event callback. */
esp_err_t masko_player_set_source(masko_player_t *p,
                                  const masko_frame_source_t *source,
                                  bool reverse, bool immediate);

/** @brief True while a queued pack is still waiting to take over. */
bool masko_player_has_pending_pack(const masko_player_t *p);

/** @brief Start looping. Returns once both tasks are running. */
esp_err_t masko_player_play(masko_player_t *p);

/** @brief Stop and join. */
void masko_player_stop(masko_player_t *p);

void masko_player_destroy(masko_player_t *p);

masko_stats_t masko_player_stats(const masko_player_t *p);

#ifdef __cplusplus
}
#endif
#endif /* MASKO_PLAYER_H */
