# ESP32-S3 streaming diagnostics — 2026-10-24

This folder preserves the available `masko_*.log` captures from the session. Files include serial monitor output, build output, and flash output. They are kept as complete text captures; likely credential fields and Masko API-key patterns are redacted if found. The original temporary captures were scanned and no credential matches were detected.

The most relevant runs for the cache and playback diagnosis are:

- `logs/masko_diag_run1.log` — one-second instrumentation before next-loop prefetch.
- `logs/masko_prefetch_run1.log` — first next-loop prefetch run; contiguous PSRAM allocation behavior.
- `logs/masko_prefetch_run2.log` — prefetch-only rolling-slot release; large contiguous cache allocation still failed for a loop.
- `logs/masko_prefetch_run3.log` — final tested chunked PSRAM cache and next-loop prefetch run; source for the final measurements.

Other captures retain earlier hotspot, home Wi-Fi, TCP-window, probe, build, and flash sessions. Filenames are unchanged from their temporary capture names to preserve provenance. The parent `REPORT.md` interprets the runs and explains limitations; no new board tests were run for this report.
