# Validation record — 25 October 2026

This is an initial implementation intended for bench validation. It has not been connected to a physical ESP32-S3 or Creality printer in this environment.

| Check | Result |
| --- | --- |
| Reference firmware compile/link | PASS — PlatformIO Espressif32 6.10.0, Arduino-ESP32 2.0.17, ArduinoJson 7.3.1, ESP32-S3 DevKitC target with OPI PSRAM overrides |
| Build size | Flash 978,841 bytes / 3,145,728-byte app partition; static RAM 47,300 / 327,680 bytes. Runtime allocations and task stacks are additional |
| Custom partition table generation | PASS — supplied 64 KiB NVS / 3 MiB application table accepted by ESP32 build tools |
| Backend automated integration tests | PASS — 8 tests |
| Clean Python environment install | PASS — requirements installed independently; `pip check` reports no broken dependencies |
| Actual HTTP service smoke test | PASS — uvicorn startup, health, ingest acknowledgment, repeated-message deduplication, authenticated latest-state read |
| Mock printer smoke test | PASS — API-key-protected server info and object query responses |
| Generated dashboard JavaScript | PASS — Node syntax check |
| Dashboard visual/browser automation | NOT RUN — Chromium download failed; manual desktop/mobile UI review still required |
| Arduino IDE / ESP32 core 3.x compile | NOT RUN — reference build used core 2.0.17 |
| Docker image/Compose execution | NOT RUN — Docker unavailable in the execution environment |
| Dokploy deployment, real TLS chain and NTP | NOT RUN — no access to user's VPS or network |
| Physical Wi-Fi, NVS retention, reboot/outage recovery | NOT RUN — hardware/network bench tests required |
| Creality Ender 5 Max / Ender 3 V3 compatibility | NOT RUN — requires responses from the installed Moonraker builds |
| Sustained fleet/heap testing | NOT RUN — complete 24–72-hour soak test before deployment |

The automated tests cover:

1. Authentication and separation of ingest/read permissions.
2. Idempotent acknowledgment and preservation of arbitrary raw object data.
3. Out-of-order retries not regressing the latest state.
4. Reboot sequence handling and collector isolation.
5. Schema validation, unexpected-field rejection, malformed JSON, and body-size limits.
6. Historical aggregation over the requested window and null offline readings.
7. Retention pruning preserving latest state and schema reinitialization preserving the database.
8. Dashboard delivery and response security headers.

The dependency set is pinned in the supplied requirements files. The test client emits an upstream httpx deprecation warning with the selected Starlette version; tests pass. Use separate Python environments for the VPS application and optional PlatformIO tooling because their web-tool dependencies have different constraints.

No credentials or real printer/VPS data are included in this package. No VPS deployment was performed.
