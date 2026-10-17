# esp32-masko-sdk

Play Masko mascot animations on the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (466×466 round
QSPI AMOLED) with ESP-IDF.

Convert on your machine, play on the chip. The device never runs a video decoder — it
decodes baseline JPEG frames from a `.mkpk` pack and pushes them straight to the panel.

```bash
tools/masko_pack.py build mascot.mov --fps 24 --quality 9 --bg 000000 --crop -o loop.mkpk
tools/masko_pack.py verify loop.mkpk
tools/flash_pack.sh loop.mkpk                 # writes the 'masko' flash partition
idf.py -p /dev/cu.usbmodem101 flash            # in examples/waveshare_amoled_175
```

---

## Read this before you write any display code

These five things cost real debugging time and are not obvious from the API docs. All are
encoded in the code and documented in [`docs/CALIBRATION.md`](docs/CALIBRATION.md).

### 1. Never push a whole frame — you get a black screen

`esp_lcd_panel_draw_bitmap()` makes the SPI master allocate a **private DMA buffer of
`max_transfer_sz` from internal DRAM**. A full 466×466 RGB565 frame is 434,312 B, but only
~311 KB of internal DRAM was free:

```
E spi_master: setup_dma_priv_buffer(1214): Failed to allocate priv TX buffer
W anim_player: Panel push failed: ESP_ERR_NO_MEM
```

The screen stays black and the error is easy to miss. Set `max_transfer_sz` to one band
(46,600 B = 50 rows) and push in bands. Total bytes over the bus are unchanged.

### 2. RGB565 must be big-endian here

Use `JPEG_PIXEL_FORMAT_RGB565_BE`, **not** `_LE`. `esp_jpeg` writes both bytes into the
buffer in memory order and the CO5300 consumes them in that order over QSPI, expecting the
high byte first. The chip being little-endian is irrelevant. With `_LE` a yellow
background `(238, 206, 57)` renders as light green `(131, 251, 115)`.

### 3. Container alpha tags lie in both directions

| File | `stream.pix_fmt` | Container tag | Actual alpha |
|---|---|---|---|
| webm (VP9) | `yuv420p` | `alpha_mode=1` | **none** |
| mov (HEVC) | `yuv420p` | `alpha_mode=none` | **full** |

Two files reporting the *same* pixel format, one opaque and one transparent. `masko-pack`
detects alpha by **decoding a frame and inspecting the alpha plane**. Never branch on
`pix_fmt` or on an `alpha_mode` tag.

### 4. Transparency cannot be displayed — flatten it on the host

The CO5300 is RGB565 and has no alpha. Transparency only means "let the background show
through", so it must be composited onto a solid colour before the device sees it. Doing it
in `masko-pack` costs zero runtime cycles; doing it per frame on an MCU already near its
frame budget would be expensive.

### 5. JPEG quality matters more than the panel

Measured against a 24-bit reference:

| Conversion | PSNR |
|---|---|
| RGB565 (panel limit alone) | 42.8 dB |
| q=9 JPEG alone | 37.6 dB |

The panel's 16-bit limit costs *less* than the JPEG setting. Don't spend effort on RGB565
dithering — raise `-q:v` instead.

---

## Measured performance

Waveshare 1.75", ESP-IDF 5.5.5, single core, 466×466:

| Stage | Time |
|---|---|
| `esp_jpeg` decode → RGB565 | 29.4 ms |
| Banded push | 26.5 ms |
| **Serial total** | **55.9 ms → 17.9 fps** |
| **Overlapped (decode core 1 + push core 0)** | **≈ 27 fps at full size** |

Effective QSPI throughput is 16.4 MB/s, not the 20 MB/s theoretical figure.

Cropping to the mascot's bounding box is the biggest single lever — a typical mascot fills
~67% of the square, which takes the same clip from 17.9 fps to ~27 fps before any
overlap. `masko-pack --crop` does this automatically.

> Note: earlier drafts of the SDK plan estimated 3.2–4.1 Mpx/s for decode. Measured is
> **7.4 Mpx/s** — `esp_new_jpeg` uses Xtensa SIMD and is about twice as fast as the
> third-party GIF benchmarks those estimates came from.

---

## Layout

```
components/
  masko_core/     .mkpk reader + measured calibration. No ESP-IDG headers,
                  so it builds and tests on a desktop.
  masko_codec/    esp_new_jpeg wrapper, RGB565_BE out
  masko_display/  banded panel push, injected esp_lcd handle
  masko_player/   two-task playback, decode and push overlapped across cores
tools/
  masko_pack.py   build / inspect / verify / preview a pack
  flash_pack.sh   write a pack into the 'masko' flash partition
  test_masko_core.sh  host tests, no hardware needed
docs/
  FORMAT.md       .mkpk v1 wire format
  CALIBRATION.md  every measured number and how it was obtained
  PORTING.md      what to change for other panels
examples/
  waveshare_amoled_175/
```

---

## Using the SDK

```c
masko_display_cfg_t dcfg = {
    .panel     = panel,              /* from the Waveshare BSP */
    .canvas_w  = 466, .canvas_h = 466,
    .band_rows = MASKO_PUSH_BAND_ROWS,
};
masko_display_t *disp;
masko_display_new(&dcfg, &disp);

const masko_player_cfg_t pcfg = {
    .pack_data   = mmap_base,
    .pack_size   = pack_size,
    .display     = disp,
    .decode_core = 1,                /* decode on core 1, push on core 0 */
    .on_event    = on_event,
};
masko_player_t *player;
masko_player_create(&pcfg, &player);
masko_player_play(player);
```

`masko_display` takes an injected `esp_lcd_panel_handle_t`, so it never links the Waveshare
BSP and can be pointed at any panel.

### Tuning

| Constant | Where | Default |
|---|---|---|
| frame rate cap | `CONFIG_MASKO_PACK_MAX_FPS` (Kconfig) | 24 |
| band rows | `MASKO_PUSH_BAND_ROWS` | 50 |
| decode core | `decode_core` in the config | 1 |
| JPEG quality | `masko-pack --quality` | 9 |

---

## Tests

```bash
tools/test_masko_core.sh path/to/pack.mkpk    # 21 checks, no hardware
tools/masko_pack.py verify path/to/pack.mkpk  # structural validation
```

The host tests cover the pack reader against real packs, header/CRC corruption, truncated
files, geometry violations, and the concatenated-JPEG marker walker.

---

## Requirements

- ESP-IDF ≥ 5.3.1 (developed and measured on 5.5.5)
- Toolchain `xtensa-esp-elf esp-14.2.0_20260121` for IDF 5.5.5
- Python 3 with `ffmpeg`/`ffprobe` on PATH; `numpy` optional but much faster for
  bounding-box measurement
- A board with ≥ 4 MB flash and PSRAM (the reference board has 16 MB / 8 MB)

## Licence

MIT. Exported Masko assets are subject to Masko's own terms — check them before shipping
hardware.