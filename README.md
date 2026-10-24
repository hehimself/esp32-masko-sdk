# Masko ESP32 SDK

An ESP-IDF SDK for preparing and playing Masko mascot animations on ESP32 devices. It
converts source video on a development computer into compact `.mkpk` animation packs;
the device decodes baseline JPEG frames and sends RGB565 pixels to an injected display
panel.

> **Development status:** this project is still evolving and does not promise a stable
> API or compatibility between revisions. Waveshare ESP32-S3-Touch-AMOLED-1.75 playback
> and touch-driven switching have been verified on hardware. The SD source builds but
> has not yet been validated on hardware. See the component status below before choosing
> a storage path for a product.

## Features

- Host-side `.mkpk` build, inspect, preview, and verification tools.
- ESP-IDF components for pack reading, JPEG decoding, display output, playback, storage,
  and Masko canvas graphs.
- Decode and display work can run on separate cores, with configurable frame-rate and
  banded display transfer limits.
- Flash, memory, and SD pack sources through `masko_store` (SD hardware validation is
  pending).
- HTTPS byte-range frame source with bounded PSRAM caches through `masko_http`;
  board-side Wi-Fi playback validation is pending.
- Reference Waveshare example with multi-pack bundles, authored transitions, touch, and
  BOOT-button playback.

## Quick start: Waveshare reference board

### Requirements

- ESP-IDF 5.3.1 or newer; developed and measured with ESP-IDF 5.5.5.
- ESP32-S3 board with PSRAM and at least 4 MB flash. The reference board has 8 MB PSRAM
  and 16 MB flash.
- Python 3, `ffmpeg`, and `ffprobe` on your computer. `numpy` is optional and speeds up
  mascot bounding-box measurement.
- The Waveshare ESP32-S3-Touch-AMOLED-1.75 board for the example as written.

### Build and flash the example

The example discovers the SDK components from this checkout. Activate your ESP-IDF
environment, then run:

```bash
cd examples/waveshare_amoled_175
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial port (for example, `/dev/cu.usbmodem101` on macOS or
`/dev/ttyACM0` on Linux). The example includes a sample animation in its assets and
uses the Waveshare board support package.

### Prepare and install your own animation

Convert a video supported by `ffmpeg` into a pack sized for the reference 466 × 466
display. `--crop` trims the image to the mascot's visible bounds; `--bg` composites any
transparency onto a solid background before the file reaches the device.

```bash
tools/masko_pack.py build mascot.mov --fps 24 --quality 9 --bg 000000 --crop -o loop.mkpk
tools/masko_pack.py verify loop.mkpk
tools/flash_pack.sh loop.mkpk
```

`flash_pack.sh` writes the pack to the example's `masko` data partition. Connect the
board and flash the application (without erasing the pack partition):

```bash
cd examples/waveshare_amoled_175
idf.py -p PORT flash monitor
```

### Kylo offline three-state experiment

The `codex/kylo-offline-3-state` branch uses one Kylo mascot and five locally stored
packs: Rest (`jar-gazing`), Working (`specimen-notes`), Attention (`excited-wave`), and
the two Rest-to-state transitions. All clips are 384×384 at 24 fps on the 466×466
canvas. Touch or the onboard BOOT button cycles Rest → Working → Attention → Rest.
Return transitions reuse the forward clips in reverse. Switching between Working and
Attention routes through Rest because the supplied graph has no direct edge between them.

The kit's complete Kylo graph has 25 packs totaling 46.7 MB, so it cannot fit in the
example's 10 MiB `masko` partition. This experiment uses only five packs. The preparation
script downloads the five public packs, verifies their headers, clears the loop flag on
the two one-shot transitions, creates the aligned bundle, and rejects bundles over 9.5
MiB. The packs are not checked into Git; rerun the script to recreate them.

```bash
tools/prepare_kylo_offline.sh
cd examples/waveshare_amoled_175
idf.py build
idf.py -p PORT flash monitor
```

Flash the animation bundle once, separately from the application image:

```bash
tools/flash_pack.sh examples/waveshare_amoled_175/assets/mascot_bundle.mkpk PORT
```

For the FPS experiment, leave each state running for at least 30 seconds. The serial log
reports shown frames, dropped frames, underruns, achieved FPS, and average/maximum decode
and panel-push time every 30 seconds. The target is 24 fps with no drops or underruns.
Use `tools/prepare_kylo_offline.sh` to reproduce the bundle; it is generated locally and
is intentionally excluded from Git along with the exported source packs.

### Single-pack and quality comparison

For a single `.mkpk` experiment, run `idf.py menuconfig`, enable **Run a single-pack
benchmark instead of the Kylo state graph** under **Masko demo**, then rebuild and flash
the firmware. Touch/BOOT state switching is disabled in this mode. Flash one pack at a
time; the display plays it continuously and reports metrics every 30 seconds:

```bash
tools/flash_pack.sh examples/waveshare_amoled_175/assets/kylo-round-q75-24fps.mkpk PORT
```

The flash helper defaults to 115200 baud for a reliable USB transfer. Set
`MASKO_FLASH_BAUD` to override it. To show two compatible packs sequentially as one loop,
use `tools/masko_sequence.py OUTPUT FIRST SECOND`; for example, put the kit's Q90 pack
before its Q75 pack, verify the resulting pack, then flash it. The generated sequence
contains each input once, in order, before repeating.

### Kylo Wi-Fi streaming experiment (ESP-IDF 6.1)

The `codex/kylo-wifi-streaming` branch includes the kit's 21 KiB
`stream/mascot.json` delivery manifest in the Waveshare app. Its 25 distinct
`.mkpk` URLs remain on the public Masko CDN. The demo cycles all 11 Kylo states
with touch or BOOT. Each state keeps looping until the next input. It plays an
authored Rest transition in each direction, routes a change between two other
states through Rest, and reads six `reverse: true` returns from their forward
pack in reverse. The kit's Any-State edges have no video; the demo uses their
destinations for explicit requests and routes them through authored clips.

Enable **Stream the full Kylo mascot over Wi-Fi** in `idf.py menuconfig`, then
enter the SSID and password under **Masko demo**. `sdkconfig` is ignored by Git;
do not put credentials in `sdkconfig.defaults` or source files. Enable **Only
measure Wi-Fi and CDN range speed at startup** for the first board test. It
connects to Wi-Fi, logs RSSI and free RAM, fetches a 64-byte CDN range, then
fetches 256 KiB and reports latency and throughput. No animation partition
flashing or SD card is required for this mode. Disable probe-only for playback:

```bash
cd examples/waveshare_amoled_175
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

The demo logs each requested state, preparation time, and the time from input
to the **first frame actually sent to the display**. It also logs the first
frame of the target loop separately; a two-second authored transition is part
of that latter time. Every 30 seconds it reports displayed FPS, dropped
frames, underruns, streaming stalls, HTTP bytes/requests/failures, cache
hits/misses, slowest request, and current internal/PSRAM availability. Leave a
state playing for 30 seconds and test cold switches through all 11 states.
Save the monitor output and run `python3 tools/analyze_stream_log.py monitor.log`
to summarize median and 95th-percentile input-to-first-frame times separately
for transitions and their target loops.

`masko_http` issues HTTPS byte-range requests for a 64-byte header, frame
index, and frame-aligned JPEG groups. One worker refills two PSRAM cache slots
per open clip (512 KiB each by default). The player retains its three decoded
384×384 RGB565 buffers and adds two compressed-frame scratch buffers. At most
the active and upcoming clip are open. These fixed buffers peak at about
3.4 MiB of PSRAM (2 MiB compressed cache, 0.5 MiB JPEG scratch, 0.84 MiB
decoded frames), before Wi-Fi/TLS, graph, and display allocations. When a
requested frame is late, the
display holds its current picture and the player retries that frame. The
streaming path has an ESP-IDF 6.1 build and host manifest check; frame rate,
memory floor, and switch latency still need measurement on the Wi-Fi board.

Applications can use the HTTP source directly after connecting Wi-Fi:

```c
masko_http_t *http = NULL;
masko_http_clip_t *clip = NULL;
ESP_ERROR_CHECK(masko_http_new(512 * 1024, &http));
ESP_ERROR_CHECK(masko_http_clip_open(http, pack_url, &clip));
masko_http_clip_configure(clip, true, 1); // loop, normal speed
masko_player_cfg_t cfg = {
    .frame_source = masko_http_clip_source(clip),
    .display = display,
    .decode_core = 1,
};
ESP_ERROR_CHECK(masko_player_create(&cfg, &player));
ESP_ERROR_CHECK(masko_player_play(player));
```

Keep `clip` open while the player uses it. For a switch, open the next clip and
call `masko_player_set_source()` from a controller task. Close the old clip
after the new `MASKO_EV_CLIP_START` event. The Waveshare demo shows this
ownership sequence and queues player callbacks so no HTTP work runs inside a
decoder callback.

## Use the components in an ESP-IDF project

The SDK is organized as local ESP-IDF components. Clone this repository alongside your
application and add its `components` directory to `EXTRA_COMPONENT_DIRS` before including
`project.cmake`:

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32-masko-sdk/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_app)
```

Declare the components your app uses in its component registration. The reference
example's `main/CMakeLists.txt` is a working starting point:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES masko_player masko_display masko_codec masko_core masko_store masko_graph
)
```

Initialize the board's display panel, create a display adapter, open a pack through a
source, and pass the pack bytes to the player. The exact board initialization and source
setup depend on your project:

```c
masko_display_cfg_t display_cfg = {
    .panel = panel,               // initialized esp_lcd_panel_handle_t
    .canvas_w = 466,
    .canvas_h = 466,
    .band_rows = MASKO_PUSH_BAND_ROWS,
};
masko_display_t *display = NULL;
ESP_ERROR_CHECK(masko_display_new(&display_cfg, &display));

masko_source_t *source = NULL;
ESP_ERROR_CHECK(masko_src_flash("masko", &source));

masko_blob_t blob;
ESP_ERROR_CHECK(masko_source_open(source, "idle.mkpk", &blob));
masko_player_cfg_t player_cfg = {
    .pack_data = blob.data,
    .pack_size = blob.len,
    .display = display,
    .decode_core = 1,
};
masko_player_t *player = NULL;
ESP_ERROR_CHECK(masko_player_create(&player_cfg, &player));
ESP_ERROR_CHECK(masko_player_play(player));
```

Consult the public headers in `components/*/include` and the complete reference app for
event callbacks, cleanup, display initialization, and graph integration. The display
component accepts an `esp_lcd_panel_handle_t`; it does not depend on the Waveshare BSP.

## Storage and memory

`masko_store` provides pack sources for a flash partition, SD card, or an existing memory
buffer:

```c
masko_src_flash("masko", &source);                 // memory-mapped partition
masko_src_sd("/sdcard/idle.mkpk", 0, &source);     // reads pack into PSRAM
masko_src_memory(data, length, &source);            // caller-owned buffer
```

Packs are opened as complete blobs. An SD pack therefore needs enough PSRAM for the
whole pack; the SD backend rejects an undersized cache instead of returning truncated
data. Flash playback is verified. SD playback has not yet been exercised on hardware.
The new `masko_http` source uses the player's `frame_source` option and reads only the
frames needed for playback; it does not use the blob-oriented `masko_store` API.
Do not write a flash partition while playing a pack from it: flash writes can stall
execution and visibly interrupt playback.

The player uses banded panel transfers to limit internal DMA memory. Avoid allocating a
full RGB565 frame in internal DRAM: a 466 × 466 frame is about 434 KB, while the display
driver’s DMA buffer must come from a much smaller internal-memory pool. Keep large
animation data in mapped flash or PSRAM and tune band size against your panel and memory
budget.

### Authored transition playback

Use a Masko transition asset between loop clips instead of crossfading transparent
frames. `masko_player_set_pack()` validates the new pack and decodes its first frame into
a dedicated PSRAM handoff buffer before returning. The current clip keeps playing until
its loop boundary; a non-looping pack holds its final frame and emits
`MASKO_EV_CLIP_END` once. The controller can then queue the next pack from that event.
The prepared frame is presented after already queued outgoing frames, and the player
continues with the new clip without clearing the display.

In the `MASKO_EV_CLIP_END` callback, ask the graph/controller for the next edge and
call `masko_player_set_pack()` with that edge's pack. Queue the transition after its
source loop ends; queue the target loop when the one-shot transition ends.

The player requires all clips in a sequence to share the same decoded bounding-box
dimensions and display placement. The preflight buffer adds one RGB565 frame in PSRAM
and uses a separate decoder for candidate validation; the two playback buffers and
banded display transfer remain unchanged. A failed candidate keeps the current picture
and reports `MASKO_EV_ERROR`.

## Masko canvas graphs

`masko_graph` executes canvas nodes, edges, inputs, and conditions. Its state machine is
independent of ESP-IDF; `masko_graph_json` loads a Masko animation configuration using
cJSON.

```c
masko_graph_json_t parsed;
if (masko_graph_json_parse(json_text, json_len, &parsed) != MASKO_GRAPH_OK) {
    // Handle the invalid canvas; release partial parser allocations if needed.
}

masko_graph_t graph;
if (masko_graph_json_bind(&parsed, &graph) != MASKO_GRAPH_OK) {
    masko_graph_json_free(&parsed);
    // Handle bind failure.
}

masko_graph_rt_t runtime;
if (masko_graph_init(&runtime, &graph, NULL, NULL, 0) == MASKO_GRAPH_OK) {
    masko_graph_set_bool(&runtime, "happy", true);

    masko_step_t step;
    while (masko_graph_next(&runtime, &step) == MASKO_GRAPH_OK) {
        // step.edge->clip identifies the clip to play; transitions are one-shot.
    }
    masko_graph_free(&runtime);
}
masko_graph_json_free(&parsed);
```

At a loop boundary, eligible non-loop edges are checked in declaration order and the
first edge whose conditions hold is selected. A transition plays once before the graph
lands on its target node. Invalid references, undeclared inputs, unsupported operators,
and nodes without loop edges are rejected during parsing.

## Tools and documentation

| Path | Purpose |
|---|---|
| `tools/masko_pack.py` | Build, inspect, preview, and verify `.mkpk` files |
| `tools/masko_bundle.py` | Bundle two or more compatible packs for the reference demo |
| `tools/flash_pack.sh` | Write a pack to the example's flash partition |
| `tools/test_all.sh` | Run host-side tests without a board |
| [`docs/FORMAT.md`](docs/FORMAT.md) | `.mkpk` v1 file format |
| [`docs/CALIBRATION.md`](docs/CALIBRATION.md) | Hardware measurements and display behavior |
| [`docs/PORTING.md`](docs/PORTING.md) | Port the display path to another panel |

Run the host tests with:

```bash
tools/test_all.sh
tools/test_all.sh path/to/animation.mkpk
tools/masko_pack.py inspect path/to/animation.mkpk
tools/masko_pack.py verify path/to/animation.mkpk
```

The graph JSON tests use cJSON from ESP-IDF when `IDF_PATH` is set, or system cJSON
(`libcjson-dev` on Debian/Ubuntu). Pack-conversion tools additionally require `ffmpeg`
and `ffprobe`.

## Performance and display notes

On the Waveshare ESP32-S3-Touch-AMOLED-1.75 with ESP-IDF 5.5.5, the verified playback
path runs at about 24 fps with no observed dropped frames or underruns. Actual results
depend on clip dimensions, panel transfer speed, and concurrent application work.
Cropping around the mascot is the most useful first optimization; it reduces both JPEG
decode work and pixels sent to the display. The default frame cap is 24 fps.

The reference panel requires RGB565 big-endian pixel output and banded QSPI transfers.
Transparency must be flattened to a background color by the host packer because RGB565
has no alpha channel. The `--crop` option aligns bounds for JPEG's 4:2:0 blocks. See
[`docs/CALIBRATION.md`](docs/CALIBRATION.md) and [`docs/PORTING.md`](docs/PORTING.md)
before adapting the display path.

## License

The SDK is licensed under the MIT License. Exported Masko assets may be subject to
Masko's terms; check those terms before shipping them in a product.
