# Kylo Wi-Fi streaming readiness

Date: 2026-10-24

Branch: `codex/kylo-wifi-streaming`

Board target: Waveshare ESP32-S3-Touch-AMOLED-1.75C, ESP-IDF 6.1

## Completed checks

## User-visible playback report

**Tester observation (2026-10-24):** After changing to a loop, playback can
take up to about 30 seconds before it becomes smooth. It may play smoothly for
roughly a second, then stop abruptly to load, resume, and freeze again for
around a second. During the worst parts, perceived playback is closer to 2 fps
than to the 20-fps average shown in the serial summary. The experience is
therefore visibly uneven and is not ready to describe as smooth streaming.

The serial FPS is an average across each 30-second window, so it hides bursty
playback and long pauses. It should not be treated as proof of smoothness. The
corresponding logs showed substantial source waiting (3.76–12.92 seconds per
window) and recurring stream stalls. The user observation confirms that those
stalls are visible as freezes; the cache can improve playback after it fills,
but startup and cold state changes remain too slow. A visual pass criterion
should include no multi-hundred-millisecond freezes after warm-up, in addition
to reaching 24 fps.

- The SDK now supports a bounded full-loop cache in PSRAM. It fills the active
  loop through the existing single HTTP/TLS worker in 256-KiB low-priority
  range jobs, then drops that clip's two rolling range buffers and reads frames
  from the cached pack data. This avoids a second TLS client/task competing
  with display DMA memory. Transition clips remain range-streamed; only loop
  clips are cached. The configured per-loop cap is 2,304 KiB.
- The cached firmware built with ESP-IDF 6.1 and was flashed and verified on
  the Waveshare board. App image: 1,204,000 bytes, with 71% of the 4-MiB app
  partition still free. Host suite passed: pack reader 22 checks, graph 19,
  config 34, and the delivery manifest check.
- On the board, full caches completed for the 1.90-MB `jar-gazing` loop in
  18.8 seconds, the 1.76-MB `specimen-notes` loop in 14.1 seconds, and the
  2.08-MB `excited-wave` loop in 15.6 seconds. These fill times include normal
  playback requests sharing the single network worker. No HTTP failures or
  display DMA allocation errors occurred in this run. PSRAM low-water was
  about 2.67 MiB; internal heap low-water was about 50 KiB.
- A 30-second window containing active touch switches and cache fills measured
  15.4 fps, 52 underruns, 33 stream stalls and 11.04 seconds waiting for frame
  data. A later switching window measured 13.4 fps, 59 underruns, 36 stalls
  and 12.92 seconds waiting. These are mixed interactive windows, not steady
  cached-loop benchmarks. At 96.4 seconds uptime, after caches had become
  available, a further window reached 21.4 fps with 643 cache hits and 14
  misses, but it still included user-requested scene changes and 3.76 seconds
  of source wait. Therefore it does not establish sustained 24-fps cached
  playback. Keep the user’s ongoing touch test in mind when comparing these
  intervals.
- Decode and display push are not the current steady-state ceiling: their
  measured averages were about 23-25 ms and 18-19 ms per frame. As these stages
  run on separate cores and overlap, the render ceiling is calculated from the
  slower stage (about 41-43 fps), not by adding both times. The remaining gap
  comes from source availability during the measured switch/cache windows.
  A clean, uninterrupted 30-second interval with one loop fully cached is
  still needed to confirm the 24-fps target.

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
- On a home guest network, the board joined on 2.4-GHz channel 1 at about
  -70 to -76 dBm. The 256-KiB CDN probe took 1,063 ms (240.7 KiB/s) in one
  run and 1,025 ms (249.6 KiB/s) in another. An uninterrupted Rest loop ran
  at 12.4 and 12.9 fps in separate 30-second samples, with 0 decoder drops,
  68-69 underruns, 41-42 stream stalls, and about 14-15 seconds waiting for
  source data per interval. HTTP requests succeeded. The compressed loop
  needs about 460.5 KiB/s to play at 24 fps while refetching every cycle.
- From the laptop on the home network, the same public CDN 256-KiB range
  returned HTTP 206 in 0.183 seconds (about 1,403 KiB/s). The laptop and
  board were on different subnets, so this does not isolate the Wi-Fi radio;
  a same-LAN iPerf test remains pending.
- The streaming build's TCP receive window was 5,760 bytes, versus 65,535
  in ESP-IDF's ESP32-S3 iPerf example. A local-only configuration experiment
  increased only the window to 16,384 bytes. The first and second 30-second
  Rest intervals reached 12.9 and 14.3 fps, respectively. The second had
  59 underruns, 36 stream stalls, 12,931 ms waiting for data, and an internal
  heap low-water mark of 55,908 bytes. This is a modest improvement at most;
  the test did not establish a causal gain beyond Wi-Fi variation and still
  missed 24 fps. The modified configuration is ignored and contains local
  credentials; it is not committed.
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

24-fps stability with a fully cached loop and cold transition latency remain
unverified. Run ESP-IDF's iPerf2 example against a computer reachable on the
same local network to isolate the Wi-Fi link, then compare bulk HTTPS download
with the SDK's range-fetch path. The current guest/main subnet split prevents
that local comparison. The active-loop cache is implemented; its measured
memory cost fits the current PSRAM budget, while its cold-fill time and
transition/cache scheduling still need tuning and a clean steady-loop test.

## Published ESP32-S3 throughput context

Espressif's [Wi-Fi performance guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html)
reports 20 Mbit/s TCP receive in an open lab and 73 Mbit/s in a shielded box
using its iperf example. An [independent ESP32-S3 user report in the Espressif
issue tracker](https://github.com/espressif/esp-idf/issues/18683) measured
roughly 37–40 Mbit/s TCP uplink on a local router, with occasional brief
stalls; uplink is not directly comparable to this CDN download. The Kylo
`jar-gazing` loop requires about 3.77 Mbit/s of compressed data at 24 fps.
Those figures show that the ESP32-S3 radio is capable of much more than this
application currently obtains, but iperf measures local TCP rather than HTTPS
ranges from Masko's CDN. Espressif's [tuning guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html)
identifies TCP receive window, RX buffers and AMPDU window as coupled factors.
An [ESP32-S3 issue report](https://github.com/espressif/esp-idf/issues/14171)
describes a 3-to-22-Mbit/s local TCP improvement after expanding a 5,760-byte
window, although our 16,384-byte CDN experiment did not reproduce that large
gain. Another [Espressif issue](https://github.com/espressif/esp-idf/issues/15385)
reports that high iPerf throughput can coexist with slow application HTTP
transfers; its workload differs from our download path.
