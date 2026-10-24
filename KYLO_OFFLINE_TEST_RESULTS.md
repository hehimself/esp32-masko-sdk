# Kylo Offline Three-State Hardware Test

Date: 2026-10-24  
Board: Waveshare ESP32-S3-Touch-AMOLED-1.75C (ESP32-S3, 16 MiB flash, 8 MiB PSRAM)  
ESP-IDF: 6.1  
Branch: `codex/kylo-offline-3-state`

## Asset and startup checks

- Bundle: 9,511,388 bytes (9.07 MiB), below the 9.5 MiB target and 10 MiB `masko` partition.
- Five physical packs validated at startup: three loops and two authored Rest transitions. All are 97 frames, 384×384 content at 24 fps on the 466×466 canvas.
- Firmware mounted the flash-mapped `masko` partition and started playback. Free heap at app startup: 305,835 bytes internal DRAM and 8,053,216 bytes PSRAM.
- No startup pack errors or decoder allocation errors observed.

## Playback benchmark

The app reports cumulative player counters every 30 seconds; the first sample was a clean 30-second Rest run. Later samples were kept on one clip long enough to infer each loop's own 30-second delta where possible.

| Loop/state | Window | Frames shown in window | Dropped | Underruns | FPS | Decode avg / max | Panel push avg / max | Serial ceiling |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Rest (`jar-gazing`) | 30 s | 719 | 0 | 0 | 24.0 | 22.384 / 23.030 ms | 18.220 / 18.241 ms | 24.6 fps |
| Working (`specimen-notes`) | 30 s | 720* | 0 | 0 | 24.0 | 22.123 / 24.894 ms | 18.225 / 19.580 ms | 24.8 fps |
| Attention (`excited-wave`) | 30 s | 720* | 0 | 0 | 24.0 | 22.191 / 25.271 ms | 18.224 / 19.826 ms | 24.7 fps |

`*` The player logs cumulative `shown` frames rather than a per-window delta. These 720-frame intervals are derived from consecutive counters while no loop-state change occurred in the interval. Counter samples show `dropped=0` and `underrun=0` throughout the run.

## Input and transition checks

- Touch successfully changed states and logs included coordinates and target state.
- BOOT button successfully changed states; observed Working → Attention via Rest and Attention → Rest.
- Authored one-shot transitions played once and returned to the target loop. Logs showed the expected sequence of forward transition, landing loop, and reverse transition assets.
- Working ↔ Attention correctly routed via Rest (two authored transitions).
- Inputs during one-shot transitions were intentionally ignored and logged as such.
- No black-frame reports, decoder errors, or playback stalls observed in the captured serial output.

## Q75 versus Q90 export comparison

The kit's `kylo-round-q75-24fps.mkpk` and `kylo-round-q90-24fps.mkpk` packs were each flashed and measured in single-pack benchmark mode. Both are 384×384 at x=40,y=40 on the 466×466 canvas, 97 frames at 24 fps. Their visible footprint is the same; Q90's file is larger because it stores higher-quality JPEG frames.

| Export | Pack bytes | 30 s shown | Dropped | Underruns | Achieved FPS | Decode avg / max | Panel push avg / max | Serial ceiling |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Round Q75 | 2,200,463 | 719 | 0 | 0 | 24.0 | 23.683 / 24.877 ms | 18.204 / 18.260 ms | 23.9 fps |
| Round Q90 | 3,409,902 | 719 | 0 | 0 | 24.0 | 28.599 / 30.362 ms | 18.457 / 18.616 ms | 21.3 fps |

Q90 is 55.0% larger and took 20.8% longer to decode on average. The measured loop still displayed 24.0 fps with no drops or underruns during both 30-second samples. The player's `serial ceiling` is calculated from average decode + push time and does not account for overlap between the decoder and display worker; achieved FPS is the observed playback rate.

For direct viewing, `tools/masko_sequence.py` built `kylo-q90-then-q75.mkpk`: 97 Q90 frames followed by the same 97 source frames at Q75, playing full-screen in sequence at 24 fps. The 194-frame pack is 5,610,383 bytes and passes `tools/masko_pack.py verify`. It is currently loaded on the device for visual review. A short phone recording of the two halves remains useful for judging the subtle quality difference.

**User visual assessment:** After viewing the Q90 and Q75 versions back-to-back on the display, the user reports seeing no visual difference. In this test, the extra Q90 file size and decode cost did not produce a perceptible improvement for the user; Q75 is the better practical choice unless another viewer or closer inspection identifies a benefit.

The combined sequence also ran for 30 seconds at 24.0 fps with 0 drops and 0 underruns. Its decode average was 26.316 ms, panel push average 18.382 ms, and measured serial ceiling 22.4 fps. Those averages combine both quality levels; the individual measurements above show Q90's higher decode cost.

## Q75 12 fps versus 24 fps full-screen comparison

The kit's `kylo-q75-12fps.mkpk` and `kylo-q75-24fps.mkpk` were validated, flashed one at a time, and measured on the Waveshare 466×466 display using ESP-IDF 6.1 single-pack benchmark firmware. Both are full-screen 464×464 loops at Q75; the 12 fps export has 48 frames and the 24 fps export has 97. This changes frame cadence and file size together, but keeps quality, animation, and image geometry matched. Do not compare these results directly with the earlier 384×384 round Q75/Q90 packs: the full-screen packs push about 47% more pixels per frame.

| Export | Pack bytes | Frames | 30 s shown | Dropped | Underruns | Achieved FPS | Decode avg / max | Panel push avg / max | Serial ceiling |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Full-screen Q75 12 fps | 1,450,237 | 48 | 359 | 0 | 0 | 12.0 | 30.886 / 32.533 ms | 26.473 / 26.491 ms | 17.4 fps |
| Full-screen Q75 24 fps | 2,937,601 | 97 | 719 | 0 | 0 | 24.0 | 31.117 / 32.804 ms | 27.530 / 27.761 ms | 17.1 fps |

Both packs played at their target rates without dropped frames or underruns in the 30-second samples. The 24 fps version is visibly smoother because it presents twice as many frames per second, with a cost of about 2× the pack size and panel updates. Per-frame decode and push timings are similar, so the FPS increase is primarily cadence rather than a harder per-frame workload. The serial ceiling is average decode plus push and ignores the player's decoder/display overlap; measured playback remained at the pack rate. The display currently has the Q75 24 fps pack loaded in benchmark mode.

**User size assessment:** In the user's recorded video, the full-screen 464×464 avatar sometimes felt a little too large. Treat this as a subjective framing preference, not a playback or image-quality failure.

**User visual assessment of the direct size A/B:** The full-screen image looks good and sharp, but parts of the avatar such as the ears are cut off by the round display. The smaller 384×384 centered version shows more of the complete character and looks a little better to the user overall. Prefer the smaller centered framing when the priority is keeping the whole mascot visible; the larger export remains a valid option when maximum apparent size and sharpness are preferred.

### Direct size A/B currently on the device

Because the `.mkpk` header defines one bounding-box size for every frame, the two original exports cannot be concatenated as-is. `kylo-q75-small-then-full.mkpk` is a 194-frame, 24 fps comparison: the first 97 frames are the 384×384 Q75 pictures centered at x=40,y=40 on a black 464×464 canvas; the next 97 frames are the original 464×464 Q75 pictures. The smaller JPEGs were re-encoded at Q75 after padding; this can introduce a small extra quality difference, though it does not scale the avatar. The full-size JPEGs are reused unchanged. The resulting 5,277,953-byte pack passed `tools/masko_pack.py verify`, was written to the `masko` partition, and the serial log confirmed 194 frames at 24 fps playing. The device is left on this looping A/B for the user's framing assessment; single-pack benchmark mode has touch and BOOT state switching disabled.

## 75 fps feasibility and next experiment

The verified pipeline is currently capped at 24 fps by `CONFIG_MASKO_PACK_MAX_FPS=24` and the assets themselves are 24 fps. More importantly, measured decode plus panel-push work is about 40.6 ms per frame, giving a measured serial ceiling of only 24.6–24.9 fps. A 75 fps target permits 13.33 ms per frame, so this current full-resolution QSPI path cannot reach 75 fps by changing the cap alone. The Waveshare BSP uses CO5300 QSPI with a 40 MHz pixel clock. The controller component also contains 60 Hz MIPI-DSI timing presets, but those do not establish that the board's QSPI path can scan or update at that rate.

Recommended next steps:

1. Keep 24 fps as the reliable full-resolution target for this export and leave the FPS cap unchanged.
2. The direct size comparison has now been generated, flashed, and confirmed running. It shows the 384×384 Q75 frames centered on a black 464×464 canvas for 97 frames (about 4 seconds), then the 464×464 full-screen Q75 frames for 97 frames (about 4 seconds), looping at 24 fps. The smaller frames were padded and JPEG-reencoded at Q75 to fit the pack's single-geometry format; the full-size frames were reused. Ask the user to assess framing from the on-device sequence/video before choosing an export size.
3. After the framing choice, compare raw RGB565 + LZ4 against JPEG as proposed in the kit README. It may reduce JPEG decode work, but full-screen panel push already takes about 26–28 ms, so it cannot reach 75 fps by itself. The raw 10.9 MB clip also exceeds the 10 MiB flash partition, so use the provided SD/stream path or a smaller export rather than trying to flash the entire raw clip there.
4. Then prioritize the deferred Wi-Fi stream experiment using `stream/mascot.json`. The whole graph is 46.7 MB and does not fit in flash; use byte-range requests and preload/chunk frames. This requires an HTTP-backed pack source in the SDK, which does not exist yet.
5. If higher frame rates remain a goal, optimize and profile decode separately from display transfer, and investigate smaller content boxes / fewer pixels and safe QSPI transfer batching. Only then try a higher-FPS export and cap; the current 26–28 ms full-screen push path is already slower than a 13.33 ms 75 fps frame budget.

75 fps is therefore not a practical next test for this pipeline as configured. The measured bottleneck is the combined decode and display transfer, not just the panel's nominal refresh rate.

## Overall test plan status and what remains

### Completed on the current SDK/player

- Validated and played the five-pack Kylo offline graph: Rest, Working, Attention, and two authored transitions. Touch and BOOT switching and return-to-loop behavior were checked at 24 fps.
- Measured the chosen 384×384 centered Q75 loop for 30 seconds: 24 fps with 0 drops and 0 underruns.
- Compared Q75 vs Q90 at the same 384×384 geometry and 24 fps. Both played cleanly; the user saw no meaningful visual difference, while Q90 used 55% more storage and took longer to decode.
- Compared full-screen Q75 at 12 vs 24 fps. Both hit their target without drops, but the user found 12 fps unpleasant to watch; 24 fps is the practical choice.
- Compared the same Q75/24 fps animation at 384×384 centered and 464×464 full-screen. The user liked the sharp large image, but the round panel cuts off details such as the ears; the smaller centered image shows more of the character and is preferred overall.
- Verified the 10 MiB flash partition and 8 MiB PSRAM constraints. The current device is left looping the small-then-full visual comparison in single-pack benchmark mode; controls are disabled in that mode.

### Can be done with the existing JPEG `.mkpk` player

- Repeat any Q75/24 fps visual or 30-second playback measurement at the preferred 384×384 framing. These packs are already supported and fit in flash. The SDK has an SD-card `.mkpk` source in `masko_store`, but there is no SD card available, so SD playback cannot currently be tested. Even with a card, its current source reads a complete pack into PSRAM, so pack size must fit the available PSRAM.
- Once visual review of the current size A/B is finished, restore the normal three-state app firmware and bundle so touch/BOOT interaction works again.

### Requires SDK or hardware work first

- **Wi-Fi streaming:** not implemented as a pack source yet. Add HTTP range/chunk reads and a bounded frame cache, then test the public `stream/mascot.json` graph; the 46.7 MB graph cannot fit in flash or PSRAM as a whole.
- **RGB565+LZ4, indexed 16-colour, transparent LVGL, AVI, or raw MJPEG:** the kit includes these exports, but the current pack parser/player accepts baseline JPEG frames in `.mkpk`. These formats need a matching decoder/reader and a playback path before they can be benchmarked. The 10.9 MB RGB565+LZ4 archive is the only provided RGB565 export and exceeds the 10 MiB flash partition; the smaller indexed and JPEG/MJPEG archives are not automatically playable by the current SDK either.
- **75 fps:** no need to try the cap alone; full-screen panel push measured 26–28 ms per frame, already exceeding a 75 fps frame budget. Revisit only after profiling/optimizing the display transfer or using a smaller frame area.

**Recommended order:** finish the user's review of the 384-vs-464 sequence; return the device to the normal 3-state build; then implement and test Wi-Fi streaming as the next substantive playback experiment. There is no SD card available, so skip SD testing. Choose a decoder-format experiment only if reducing decode cost is still a priority after streaming or if the user wants to evaluate visual trade-offs.
