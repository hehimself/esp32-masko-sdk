# ESP32-S3 live animation streaming: diagnostic report

**Date:** 2026-10-24  
**Device:** Waveshare ESP32-S3-Touch-AMOLED-1.75C  
**Firmware:** ESP-IDF 6.1, Masko streaming demo, `codex/kylo-wifi-streaming`  
**Evidence:** board serial captures, app diagnostics, previous CDN range probes, and the user's visual observations. The complete available logs are in [`logs/`](logs/).

## Executive finding

The display path and JPEG decoder can play the tested loops at about 24–25.5 fps once the complete loop is resident in PSRAM. Cold streaming is the problem: range requests and the single HTTP worker cannot reliably deliver frames at playback cadence, producing long freezes and large one-second FPS swings. Authored transition clips remain range-streamed, so they still stutter even when the destination loop has already been prefetched.

This is not evidence that the ESP32-S3 display or decoder is inherently too slow. It is evidence that the current playback path cannot guarantee smooth frame delivery from the network in real time. The best immediate architecture is *fetch ahead, then play from memory*: cache the visible loop and expected next loop, and eventually prewarm transitions when memory permits. The current firmware has only the loop prefetch portion of that design; transitions have not yet been prefetched in tested firmware.

## What was measured

| Situation | Observed result | Interpretation |
|---|---|---|
| First `jar-gazing` loop cache fill | 1,904,796 bytes; approximately 18.8–22.4 s in captures, depending on run | Startup playback remains uneven while data is arriving. The variation is real across runs and includes shared-worker scheduling. |
| Cold loop while cache is filling | One-second diagnostic windows ranged from 0 fps to near 24 fps; recorded maximum display gaps reached roughly 5.2 s in the most severe captured run | A 30-second average hides the visible burst/freeze behavior. Cold playback is not smooth. |
| Fully cached `jar-gazing` / `specimen-notes` / `excited-wave` | Repeated one-second windows typically around 24.1–25.5 fps; source wait about 10–18 ms per second; zero stream stalls and queue underruns in steady windows | Cached loop playback meets the 24 fps target in the observed warm intervals. These were diagnostic intervals, not a newly run formal 30-second acceptance test. |
| Next-loop prefetch | `specimen-notes` (1,756,921 B) became ready in roughly 4.4–6.2 s in later captures; `excited-wave` (2,078,107 B) in about 5.3 s; `jar-present` (2,235,253 B) in about 5.2 s | Background prefetch works while the current cached loop stays smooth. Actual fill duration varies with request scheduling/network conditions. |
| Transition playback | Transition windows included 0–2 fps, multiple stalls, and multi-second display gaps even while the destination loop cache was ready | The transition itself is the remaining live-stream bottleneck. A cached destination cannot compensate for an uncached transition clip. |
| Input-to-first pixel | Direct transition first pixels were about 0.9–1.2 s in captured runs; two-hop route's final transition began about 11.9 s after input because the first authored transition and route duration occur first | Separate transition-start latency from total time to destination loop. For the latter, authored transition length is part of the wait. |
| Processing time on cached frames | Decoder about 21–24 ms/frame; panel push about 18–19 ms/frame | Neither appears to be the dominant bottleneck for current 24 fps assets when the source is ready. These are stage timings and can overlap; do not add them as a strict frame time. |
| Memory | Contiguous allocation of a 2.078 MB loop failed despite adequate total free PSRAM; 256 KiB chunked allocations succeeded. With active and prefetched caches, observed free PSRAM was about 1.98 MiB and internal free heap about 100–115 KiB. | PSRAM fragmentation, rather than total capacity alone, blocked the earlier allocation strategy. Chunked cache allocation addressed this specific failure. Internal heap remains a resource to monitor. |

### Network interpretation

A prior desktop probe of the CDN delivered about 509.7 KiB/s on one path. The 1.90 MB `jar-gazing` loop would need about 460.5 KiB/s continuously at 24 fps if every cycle were downloaded again. That comparison has little safety margin and is not a device-side sustained benchmark. Earlier ESP32 home-network probes were around 241–250 KiB/s, while earlier hotspot probes were lower still. The measured app path also showed hundreds of milliseconds to multiple seconds for range jobs, with queue wait under worker contention.

So the practical conclusion is precise: the tested network/app path is not reliable enough for uncached real-time playback, although it can fill a background cache and then support smooth playback. The evidence does **not** isolate the radio from CDN route, HTTPS/TLS, range-request behavior, or worker scheduling. A same-LAN iperf run would measure radio/TCP capacity, but it would not by itself prove the CDN range path is fast enough.

## Root-cause assessment

1. **Primary: frame availability from the HTTP source.** Source waits, underruns, and pauses rise during cache fill and transitions; they fall close to zero after a full cache is ready.
2. **Amplifier: one non-preemptive HTTP worker.** A large background cache request can occupy the worker while frame range jobs wait, and foreground range jobs can delay background cache jobs. Request queue delay is therefore part of end-to-end latency, even when raw Wi-Fi throughput looks reasonable.
3. **Transition assets are still streamed live.** The loop-only cache intentionally leaves one-shot transition clips on range reads. Their stuttering is consistent with the source-delivery problem, rather than proof that the authored animation is broken.
4. **Memory fragmentation affected the first cache implementation.** A large single PSRAM allocation failed despite sufficient aggregate free PSRAM. Splitting the full cache into 256 KiB chunks fixed allocation for the tested loops. Two simultaneous full clips fit in observed runs, but memory headroom is limited and can vary with Wi-Fi/TLS and display allocations.
5. **Decode and panel push are secondary for these packs.** Warm cached loops reached the requested frame rate. Optimizing JPEG or panel transfers first is unlikely to fix the observed cold-stream stalls.

The tester's earlier report of up to roughly 30 seconds before smoothness, brief smooth bursts followed by approximately one-second freezes, and perceived worst-case playback near 2 fps matches the cold-stream diagnostic pattern. It should remain labeled a user observation, not a calculated frame-rate sample.

## Improvement plan, ordered by likely benefit

### 1. Keep prefetching the next loop, but make readiness visible

The tested firmware prefetches the next expected touch loop while the active loop plays from its cache. Continue to use this as the baseline. At boot, consider a clear loading state or hold the first still frame until enough initial frames are buffered; do not present a partly fed loop as though it were smooth. Show a brief visual loading cue for a requested state if its transition/loop is not ready.

### 2. Prefetch transitions as well as loops

Transition clips are the strongest remaining playback defect. Measure their byte sizes first, then cache the next expected transition in PSRAM before user input. If memory allows, cache the transition plus destination loop, and for routes through Rest cache the two transitions one at a time (or at least the imminent transition). Use chunked PSRAM storage, explicit memory budgeting, cache eviction, and a safe live-stream fallback. Do not assume that caching every edge in the 25-pack graph fits. Current measured free PSRAM after two loop caches was only about 1.98 MiB.

A transition-prefetch implementation was started after the last test but was not built or flashed. It has been removed from the demo source so the checked-in behavior corresponds to the tested loop-prefetch firmware.

### 3. Make HTTP work schedulable at frame granularity

The current worker processes range jobs serially. Instrument queue wait and service-time percentiles per job class, then reduce the maximum uninterrupted cache job duration or split cache downloads into smaller ranges. Keep frame reads responsive while still giving background cache jobs enough service to finish. Test chunk sizes empirically (for example 32, 64, 128, and 256 KiB), with the same clip and network, comparing both cache-fill time and transition/frame stalls.

Avoid adding a second TLS client/task as the first fix: earlier runs showed internal heap and display DMA pressure, and a second client duplicates buffers. First benchmark a cooperative single-worker scheduler and quantify required internal/DMA heap headroom.

### 4. Treat buffer sizing and transport tuning as experiments

The standalone CDN probe, local iperf, and SDK range player measure different paths. Record RSSI/channel, same-LAN TCP throughput, TLS handshake cost, HTTP range service time, queue time, bytes per second, and request failures separately. Then tune receive buffers/TCP window, HTTP receive buffer, connection reuse, and TLS session reuse one at a time. Validate improvements with the actual repeated range workload, not only iperf. The earlier larger TCP-window trial was only a modest change and did not reach 24 fps.

### 5. Reduce the amount of data when instant switching matters

Offer smaller authored exports: lower resolution, shorter loops, and Q75 where visual comparison showed no noticeable difference from Q90. The 12 fps full-screen variant was reported as unpleasant to watch; retain 24 fps and optimize bytes per frame/geometry instead of lowering cadence. Keep the avatar small enough to avoid clipping ears, based on the user's prior framing review. A smaller clip shortens startup/prefetch time and allows more states to stay cached.

### 6. Add a cache policy and measurable acceptance criteria

Track total/contiguous PSRAM, chunk count, cache evictions, cache readiness at touch, and per-class request latency distributions. Define a memory budget for active loop + imminent transition + next loop, with eviction/fallback when it is exceeded. Acceptance should require: warm-loop 24 fps for at least 30 seconds; no underruns or visible multi-hundred-millisecond gaps; transition frame cadence measured separately; and cold-switch-to-first-frame and cold-switch-to-smooth-loop reported separately.

## Limits and next useful experiment

No further board test was run for this report. The final tested implementation is loop prefetch with chunked PSRAM allocation and diagnostics. The most informative next engineering iteration is to implement bounded, priority-aware range scheduling and transition prefetch, then compare cold transition playback against the captured live-stream baseline. For attribution, pair that with a same-LAN iperf run and a repeated SDK HTTPS range benchmark. Do not claim Wi-Fi as the sole root cause until those paths are separated.
