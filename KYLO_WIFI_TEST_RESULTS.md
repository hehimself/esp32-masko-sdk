# Kylo Wi-Fi streaming readiness

Date: 2026-10-24

Branch: `codex/kylo-wifi-streaming`

Board target: Waveshare ESP32-S3-Touch-AMOLED-1.75C, ESP-IDF 6.1

## Completed checks

- The kit delivery file contains 11 states, 42 graph edges and 25 distinct
  `.mkpk` URLs. The host parser recognizes all 25 URLs, six URL-less reverse
  returns, eleven URL-less Any-State edges and three `>` time conditions.
- Each of the 25 CDN URLs answered a 64-byte HTTP Range request with `206
  Partial Content`, a matching `Content-Range` and `MKPK` magic. This is a
  desktop network check, not a board-side Wi-Fi result.
- The `jar-gazing` pack additionally passed header CRC validation; its 776-byte
  frame index and first 20,285-byte indexed JPEG frame were retrieved using
  separate Range requests. The JPEG start and end markers were valid.
- The Waveshare streaming configuration builds with ESP-IDF 6.1. The host
  graph tests, actual Kylo manifest check, and `.mkpk` pack reader tests pass.
- On 2026-10-24 the probe-only ESP-IDF 6.1 firmware was flashed to the
  Waveshare board. It booted and parsed the 11-state, 42-edge manifest. After
  the mobile hotspot became discoverable, the board joined it at -67 dBm.
  Free heap after Wi-Fi: 210,751 bytes internal and 7,416,936 bytes PSRAM.
- The first CDN request failed TLS verification because the CDN served a
  cross-signed Google Trust Services chain. Enabling ESP-IDF's cross-signed
  certificate bundle verification fixed HTTPS without disabling certificate
  checks. The device then retrieved 64 bytes in 1,173 ms and 256 KiB in
  4,640 ms (55.2 KiB/s). This is one hotspot measurement, not yet a stable
  streaming benchmark. The serial port was closed after the test. No Wi-Fi
  credentials were committed.
- Full playback over the mobile hotspot did reach the `jar-gazing` first pixel.
  The first 30-second line reported 2.8 fps, 83 displayed frames, no decoder
  drops, 131 underruns, 85 stream stalls, 26,570 ms spent waiting for source
  frames, and zero HTTP failures. The HTTP transfer was 1,963,209 bytes across
  ten requests, with an 8,829-ms slowest request. The displayed-FPS figure
  includes a transition request late in the interval; it still demonstrates
  that this hotspot did not sustain 24 fps. Free internal heap at the sample
  was 93,047 bytes, but its earlier low-water mark was only 8,312 bytes.
- The original 512-KiB cold range made the first frame time out at eight
  seconds. The HTTP source now fetches a small first range and uses full cache
  slots afterwards. The example's main-task stack was increased to 8 KiB after
  a real board stack-overflow reset. Mbed TLS allocation was moved to PSRAM to
  improve the internal-memory low-water mark.
- After the TLS memory change, an uninterrupted `jar-gazing` loop reached
  3.2 fps over 30 seconds: 97 displayed frames, no decoder drops, 129
  underruns, 82 stream stalls, 25,877 ms waiting for source frames, seven HTTP
  requests, zero HTTP failures, and an 8,353-ms slowest request. The 256-KiB
  probe measured 62.4 KiB/s. Current free heap was 125,567 bytes internal and
  4,772,256 bytes PSRAM; low-water marks were 71,228 and 4,771,708 bytes.
  The 1,905,820-byte, 97-frame, 24-fps loop needs about 460.5 KiB/s sustained
  download if it is refetched each cycle, roughly 7.4 times this hotspot's
  measured rate. The bandwidth ratio predicts about 3.25 fps, closely matching
  the measured 3.2 fps. This strongly points to delivery throughput, not the
  JPEG decoder, as the current bottleneck. It does not by itself distinguish
  the phone's internet path, the ESP32's Wi-Fi link, CDN latency, or the HTTP
  client implementation.
- A laptop on a separate network fetched the same 256-KiB CDN byte range in
  0.180 seconds (about 1,425 KiB/s, HTTP 206), showing that the CDN can serve
  this pack faster on at least one other path. This is not an ESP32 benchmark.
## Board test after Wi-Fi is configured

1. Enable `MASKO_STREAM_ENABLE`, set SSID/password locally in `idf.py
   menuconfig`, and first enable `MASKO_STREAM_PROBE_ONLY`. Flash and record
   RSSI, internal/PSRAM free memory, 64-byte request latency and 256-KiB
   transfer rate. A failed probe stops before playback.
2. Disable probe-only, rebuild and flash. Leave Rest running for 30 seconds.
   Cycle through all 11 states with touch and BOOT. Wait for each loop to
   appear before the next touch. Run each loop for 30 seconds.
3. Record the per-switch `first pixel` lines and each 30-second metrics line.
   Separate input-to-transition-frame from input-to-target-loop-frame: the
   latter includes the authored transition's duration.
4. Watch for drops, underruns, stream stalls, request failures and low water
   marks for both memory types. Review reverse return transitions visually.

## Pending measurements

24-fps stability and cold transition latency remain unverified on a connection
fast enough to supply the JPEG data. Repeat the CDN probe and all-state
playback test on a faster 2.4-GHz Wi-Fi network.

## Published ESP32-S3 throughput context

Espressif's [Wi-Fi performance guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html)
reports 20 Mbit/s TCP receive in an open lab and 73 Mbit/s in a shielded box
using its iperf example. An [independent ESP32-S3 user report in the Espressif
issue tracker](https://github.com/espressif/esp-idf/issues/18683) measured
roughly 37–40 Mbit/s TCP uplink on a local router, with occasional brief
stalls; uplink is not directly comparable to this CDN download. The Kylo
`jar-gazing` loop requires about 3.77 Mbit/s of compressed data at 24 fps.
Those figures make home Wi-Fi plausible, but iperf measures local TCP rather
than HTTPS ranges from Masko's CDN. A board-side home-network test is still
needed to establish actual frame rate and switch latency.
