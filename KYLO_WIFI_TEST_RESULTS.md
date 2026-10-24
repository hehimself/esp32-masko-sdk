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

Actual device-side HTTPS throughput, 24-fps stability, cold transition
latency, warm loop behavior and memory floor are unmeasured until the board
has WLAN credentials. The firmware has not been flashed in this branch.
