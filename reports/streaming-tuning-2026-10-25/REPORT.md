# Kylo Wi-Fi streaming tuning on ESP32-S3

- **Date:** 2026-10-25
- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75C
- **SDK branch:** `codex/kylo-wifi-streaming`
- **Toolchain:** ESP-IDF 6.1
**Evidence:** [12 sanitized serial captures](logs/) from the connected board. The report describes measured firmware, not an 11-state smoothness guarantee.

## Result

The selected firmware uses the measured Wi-Fi/lwIP profile, a **4 KiB HTTP receive buffer**, **8-row display transfers**, a **one-entry SPI transfer queue**, **64 KiB internal DMA reserve**, hardware AES, and chunked PSRAM caching. The initial Kylo `jar-gazing` loop, anticipated `jar-gazing → specimen-notes` transition, and destination loop loaded in 6.087, 2.788, and 2.343 seconds, respectively, in the controlled run. That run displayed **23.9 fps with no dropped frames or underruns during the first 30 seconds**, then **24.0 fps with no drops or underruns during the next 30 seconds**. It had no DMA allocation, AES allocation, player, or HTTP request failures. The second window had zero HTTP traffic.

The first prefetched transition appeared **74 ms after touch** in the separate input run. The destination loop's first displayed frame was **2.125 seconds after touch**, including the approximately two-second authored transition. Three touch switches in that capture produced no memory or HTTP errors. A rapid BOOT sequence before caches were ready was much slower: the first transition frame took **3.222 seconds** and its 30-second interval averaged **14.6 fps**, with 40 underruns, 25 streaming stalls, and one panel DMA allocation failure. The button worked, but cold/rapid switching still does not meet the 24 fps acceptance target.

The selected firmware is built and flashed on the connected board. No Wi-Fi credential or certificate was added to tracked configuration or logs. Serial captures redact the configured SSID, password, MAC addresses, and IP addresses.

## Visible pause at the loop boundary

The user observed a short pause whenever a loop restarts. The player does **not** intentionally wait at a loop boundary: after displaying the last frame, it sets the frame index to zero and continues on its absolute frame schedule. Its `MASKO_EV_CLIP_END` callback is sent through a nonblocking queue, and the demo ignores that event for a looping clip. The controlled cached `jar-gazing` run showed 24.0 fps, zero dropped frames, zero underruns, zero stream stalls, and zero HTTP traffic in its second 30-second window. Its cumulative maximum display-start gap was 84 ms, with no gaps over 100 ms; the log does not say whether that 84 ms occurred at a loop boundary.

I separately downloaded the public `jar-gazing` MKPK referenced by the bundled manifest and inspected its frame index and JPEG payloads. It has **97 frames at 24 fps**, so it lasts **4.0417 seconds**, one frame longer than the manifest's nominal four seconds. **Frames 95 and 96 are byte-for-byte identical JPEGs** (both SHA-256 prefix `614e231bb0a57379`). The complete pack SHA-256 was `28b9817a478303a50f8c11d4f69c0c905e9f88ebae404f540488442c7309fed8`. Thus the final image is held for about **83 ms across two frame intervals**, compared with about 42 ms for one ordinary frame. The following last-to-first image difference is in the normal range of adjacent image differences, so the duplicate terminal frame is the strongest identified explanation for a subtle end-of-loop hold. It is an asset property, not a network refill or an explicit player sleep.

This establishes a likely cause for the observed `jar-gazing` pause, but the serial trace lacks frame-index timestamps at the wrap and cannot prove that every perceived pause has this cause. Other Kylo loops were not inspected for duplicate terminal frames. The next asset-level experiment is a 96-frame export with the duplicate terminal image removed, then a side-by-side visual comparison on the board. The player should remain unchanged until that comparison distinguishes an authored hold from a scheduling gap.

## Measurement setup

The same public `jar-gazing` pack was used for startup probes in each configuration (1,905,820-byte remote file; 1,904,796 bytes of compressed frame data). The three probe requests were cold 256 KiB, reused-connection 1 MiB, and new-connection 256 KiB. `body_Mbit_s` uses Mbit/s, not MB/s, and excludes the delay before the first response byte. RSSI was approximately −44 to −54 dBm, on channel 6, BW40, with Wi-Fi power save disabled. The first byte of a fresh connection usually took roughly 0.5–0.65 s; a reused connection was commonly tens of milliseconds. One BOOT-run reused request took 0.566 s, showing that reuse is not a guarantee.

The Wi-Fi lab's separate local HTTP range tests reached **14.3–21.2 Mbit/s** and its Masko CDN tests reached about **3.8–4.1 Mbit/s**. Those tests establish that the board's local radio path can exceed the older CDN measurements. They do not isolate which network or server condition changed between the lab and today's 6–9 Mbit/s SDK probes. The application measurements below are the relevant comparison for playback.

## Before and after

| Firmware variant | Reused 1 MiB body rate | First loop cache fill | Playback / memory observation |
|---|---:|---:|---|
| Original SDK network profile, 4 KiB HTTP, 50-row display, queue 10 | 2.20 Mbit/s | 22.071 s | First 30 s: 10.9 fps, 59 drops, 68 underruns; 144 panel DMA allocation errors in the capture. |
| Only HTTP receive buffer raised to 16 KiB | 2.11 Mbit/s | 22.294 s | No throughput or cache-fill gain; roughly 12 KiB less internal heap during probe. |
| Larger TCP window/mailboxes alone, 4 KiB HTTP | 1.76 Mbit/s | Input interrupted comparison | No improvement; this single change should not be described as the cause of later gains. |
| Grouped lab-reference Wi-Fi/lwIP profile, 16 KiB HTTP, 50-row display, queue 10 | 8.90 Mbit/s | 9.640 s | Transport improved, but panel DMA errors rose to 700 in the capture; the display needed less transient internal memory. |
| Same grouped profile, 16-row display, queue 10 | 8.48 Mbit/s | 6.860 s | 138 DMA errors in a run with touch switches. |
| 16-row display, queue 2, no transition prefetch | 8.38 Mbit/s | 4.433 s | Nine DMA errors near cache fill; untouched second 30 s: 24.0 fps, zero drops/underruns. |
| 16-row display, queue 1, transition prefetch | 9.14 Mbit/s | 5.268 s | First prefetched transition: 95 ms; three simultaneous caches left ~0.4 MiB PSRAM, with 120 DMA and 11 AES allocation errors across the run. |
| Software AES instead of hardware AES | 3.68 Mbit/s | 14.985 s | Removed AES-DMA allocation errors but slowed the CDN path and still had 180 panel DMA errors. Rejected. |
| Hardware AES, 8-row display, queue 1, 64 KiB DMA reserve, 16 KiB HTTP | 8.79 Mbit/s | 5.485 s | Improved transient DMA availability but 21 panel and four AES errors during later switches. |
| **Selected:** same display/memory profile, **4 KiB HTTP** | **6.02 Mbit/s** in controlled run; **8.62 Mbit/s** in touch run | **6.087 s** | Untouched first/second 30 s: **23.9/24.0 fps**, zero drops/underruns/errors. Three touch switches: zero memory/HTTP errors. |

The grouped Wi-Fi/lwIP change includes 16 static RX buffers; 64 dynamic RX and TX buffers; AMPDU TX/RX and BA windows of 32; Wi-Fi IRAM options; 65,535-byte TCP send/receive windows; 64-entry TCP mailboxes; and lwIP IRAM optimization. mbedTLS external allocation was already present. The group and network conditions changed together, so the 2.20 → 8.90 Mbit/s improvement cannot be attributed to one knob. Neither TCP window scaling nor PSRAM-preferred Wi-Fi/lwIP allocation was enabled: the standalone lab had not shown a Masko gain from those trials.

At Wi-Fi association the selected firmware had approximately **156 KiB free internal RAM**, an **80 KiB largest DMA-capable block**, and **7.26 MiB free PSRAM**. After the three selected packs were cached, free PSRAM was about **0.41 MiB** and internal RAM about **103 KiB** in the controlled run. The cumulative minimum internal heap was **7.8 KiB** during overlapping startup/network work, so the successful run has little worst-case margin. The fixed 8-row transfer is 6,144 bytes, versus 46,600 bytes for the earlier 50-row transfer. Warm per-frame decode and display push averaged about 22.6 ms and 25.9 ms, respectively, in the second 30-second window; those stages run on separate tasks and should not be added as a serial frame time.

## Where time and failures went

The HTTP worker still has one client and prioritizes foreground frame jobs before 256 KiB background cache jobs. In the original run, logged frame jobs had a median **249 ms queue wait** and **1,885 ms service time**; cache jobs had up to **2,127 ms queue wait**. In the selected no-touch run, logged frame jobs had a median **138 ms queue wait** and **548 ms service time**; cache jobs had a median **380 ms service time**, with up to **772 ms queue wait**. These are log subsets: the worker only logs every cache job and frame jobs taking at least 250 ms. They are not population-wide latency percentiles.

An already cached loop imposes no foreground HTTP work. In the selected run, the 1,740,227-byte anticipated transition cache filled in 2.788 seconds and the 1,756,921-byte destination loop cache in 2.343 seconds while the active loop remained smooth. The 1.90 MB initial cache required 6.087 seconds because startup playback also requested foreground frame ranges. The extra time, foreground queue waits, and cold-start stalls show worker scheduling and competing frame traffic in addition to raw CDN delivery. The BOOT capture demonstrates the consequence: pressing at 11.533 s, before prefetch readiness, yielded a 3.222-second delay to the first transition frame; two further quick state changes led to a 14.6 fps interval despite zero HTTP request failures.

The lab's one-shot full GET did not raise sustained Masko CDN rate. In the current player, replacing a 256 KiB cache job with one non-preemptive ~1.9 MB GET would keep the only HTTP worker occupied for seconds while a cold frame or transition waits. The measured 256 KiB jobs already fill an isolated anticipated clip within roughly 2–3 seconds. The full-GET path was therefore **not added** to the selected firmware. A separate cache worker/client would require its own TLS and internal-memory budget; the low observed startup heap minimum makes that a later experiment, not a safe automatic change.

## Code and configuration

- `masko_http` now logs cold/reused/fresh Range timings, first-byte delay, worker queue wait, body throughput, retries, connection reuse, errors, and memory. The probe accepts a 1 MiB response without allocating a contiguous 1 MiB PSRAM buffer. Its HTTP receive size and timeout are Kconfig settings.
- The demo logs RSSI/channel/BW and free internal/DMA/PSRAM memory. It caches the anticipated touch transition before the destination loop, reuses a ready transition from PSRAM, and falls back to live Range playback if caching is unavailable. A failed asynchronous transition cache is detected and released.
- The Waveshare profile in `sdkconfig.defaults` records the measured Wi-Fi/lwIP settings, 64 KiB internal DMA reserve, one-entry SPI queue, 4 KiB HTTP receive buffer, and 16 KiB main-task stack required by the ESP-IDF 6.1 HTTPS startup path. Board-specific flash/partition settings were preserved. Wi-Fi credentials remain only in ignored local `sdkconfig`.
- The measured local configuration also had mbedTLS MPI interrupt use disabled; the tracked defaults now match that setting. The measurements do not establish a performance effect from it.
- Display transfers use eight rows per call. The 10 MiB animation partition is unchanged; CDN packs are streamed or cached in PSRAM and are not flashed into it.

To repeat: configure the SSID/password locally with `idf.py menuconfig`, build and flash `examples/waveshare_amoled_175` with ESP-IDF 6.1, and record the startup probes and two untouched 30-second reports. Then press touch once only after `diag_transition_prefetch ... cache=ready` and `diag_prefetch ... cache=ready`. Compare `input-to-display` for the transition and the destination loop. Finally press BOOT early on a fresh boot to quantify cold behavior. Store redacted serial logs; never commit `sdkconfig` or unredacted credentials.

## Remaining limits

The 8 MiB PSRAM cannot hold the 25 packs (46.7 MB) or guarantee that the active loop, imminent transition, and destination loop always fit together. The three Rest→Working packs alone consumed about 5.40 MB of compressed clip cache and left ~0.41 MiB PSRAM after other allocations. A larger edge can fail prefetch, leaving a live Range fallback. A route between non-Rest states plays two authored transitions through Rest, so its time to the target loop includes both clip durations even if neither download stalls. The final touch capture's Working→Attention loop appeared 5.926 seconds after input; one intermediary transition was not prewarmed. The capture did not exercise all 11 states or all 25 packs.

To close the remaining gap, measure memory and cadence for every edge, then add an explicit cache eviction/just-in-time policy for two-hop routes. Cache the next hop during the first cached transition, and only switch at a clip boundary once its next clip is ready. Profile an 8 KiB HTTP receive buffer and smaller foreground Range slots as separate controlled experiments; they may improve throughput or worker preemption, but today there is no evidence that either retains the 4 KiB profile's error-free playback. Finally measure a same-LAN HTTPS source and CDN source with identical app requests to distinguish CDN pacing from TLS/TCP work on the ESP32. The current evidence supports **smooth prefetched playback**, not uninterrupted 24 fps for arbitrary cold presses.
