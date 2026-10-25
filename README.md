# PrintFleet v0.1.0

Arduino-based ESP32-S3 Moonraker LAN collector, local web dashboard, and a Docker-ready Python VPS receiver with persistent SQLite storage and a history dashboard.

**Status:** initial implementation for bench validation, not a field-qualified release. See `VALIDATION.md` for checks actually run. No printer-control commands are exposed. Test on one printer before expanding the fleet.

## What is included

| Component | Capability |
| --- | --- |
| ESP32-S3 | Up to eight printers, dynamic Klipper object discovery, bounded HTTP polling, authenticated local UI |
| Discovery | Scans the connected IPv4 subnet on a configurable port; identifies Moonraker by API fields; protected services shown as unverified candidates |
| Manual setup | IP address or LAN DNS hostname, port, API key, display name, model label, enable/disable, edit, remove |
| Live telemetry | Available temperatures/targets/power, print state, progress, filename, durations, filament use, positions, fan, sensors, MCU/system status, and other runtime objects |
| Supplementary data | Moonraker server info, Klipper info, current G-code metadata, optional host process statistics |
| Local dashboard | Printer cards, live temperature charts, raw JSON inspector, Wi-Fi/VPS/settings forms, scan progress and delivery counters |
| Persistence | NVS stores only configuration and the collector ID. Telemetry never goes to NVS or a local flash filesystem |
| VPS | Bearer-authenticated ingest, duplicate suppression, observation ordering, raw JSON storage, historical temperature/progress buckets, independent read token |
| Deployment | Compose, Dockerfile, persistent volume, health check, non-root container, limited logs, Dokploy instructions |

### Data coverage and explicit limits

Moonraker reports the objects enabled by that printer's Klipper configuration. Nothing can expose a sensor the printer does not have. Creality vendor builds may differ; a working Moonraker installation is a prerequisite, and this package does not install/root the printer.

The collector discovers object names at runtime and requests their available fields. It excludes `configfile`, `bed_mesh`, `exclude_object`, and `gcode_macro ...` objects from repeated snapshots because these may contain large geometry, configuration, or sensitive values. It supports 128 queried runtime objects, a 16 KiB HTTP response, and a 24 KiB serialized telemetry envelope. If a bulk query fails or is oversized, the collector attempts a smaller core query and reports the fallback in `error`. If the complete envelope exceeds its limit, supplementary metadata/process statistics are omitted first. If it is still too large, an explicit error is sent with an empty data object. These are deliberately bounded snapshots, not an unlimited archive of every Moonraker endpoint.

Current-file metadata excludes thumbnail descriptors; no camera/video stream, G-code console, complete file catalogue, historic Moonraker job import, or persistent WebSocket event subscription is included. Polling can miss short-lived transitions. The requested interval is best effort: unavailable printers, large responses, or many objects can extend it. Host process statistics are requested at most once per minute. `eventtime` is Klipper monotonic time, not UTC. Supplemental data may be cached; `captured_at` dates the snapshot, not the freshness of every cached field.

There is no standard guaranteed printer-model/serial field in `/printer/info`. The reported hostname and software version are retained; set the physical printer model yourself. A host CPU model is not treated as the printer model.

## 1. Hardware and Arduino IDE

Use an **ESP32-S3 with PSRAM**, preferably an **N8R8 or N16R8 module (8 MB PSRAM)**. PSRAM is required by this implementation. A board without PSRAM stops at startup and prints the reason. Use a stable USB supply. This design needs no wiring to the printers and no external SD card.

1. Install Arduino IDE 2.x and **esp32 by Espressif Systems** in Boards Manager. The reference compile uses core **2.0.17**; the sketch uses APIs also available in 3.x, but that core requires a separate compile/hardware check.
2. Install **ArduinoJson 7.3.1** in Library Manager. Wi-Fi, WebServer, HTTPClient, WiFiClientSecure, Preferences, and FreeRTOS are supplied by the ESP32 core.
3. Open `firmware/PrintFleet/PrintFleet.ino`. Keep `web_ui.h` and `partitions.csv` in the same sketch folder.
4. Select your exact ESP32-S3 board, correct physical flash size, and its matching PSRAM type. N8R8 commonly uses **OPI PSRAM**; follow your board's specification. Enable USB CDC On Boot if using native USB serial.
5. The supplied custom `partitions.csv` reserves **64 KiB NVS**, a **3 MiB application**, and uses only the first 4 MiB of flash. Keep it in the sketch folder; Arduino-ESP32 supports a sketch-local partition CSV. Choose a Tools > Partition Scheme with at least a 3 MiB app budget so the IDE's size check is consistent. Do not replace the custom CSV with a small-NVS default. Configuration is stored as an NVS **blob**, avoiding the NVS string length limit.
6. Compile and upload. Open Serial Monitor at **115200 baud**.

The optional `platformio.ini` pins the Espressif32 platform, Arduino core, and ArduinoJson for a repeatable reference compile. Its board configuration targets S3 DevKitC with OPI PSRAM; adapt board and memory type for different modules. It is still the same Arduino sketch, not a separate firmware implementation.

```bash
python -m pip install platformio==6.1.18
python -m platformio run
# With your board attached:
python -m platformio run --target upload
```

## 2. First boot and Wi-Fi

On first boot, the serial console prints:

- Username: `admin`
- A generated device-specific password
- Setup AP: `PrintFleet-<id prefix>` (same password)

Connect to that setup AP and open **http://192.168.4.1**. The browser prompts for the admin username/password. In **Settings**, enter your 2.4 GHz Wi-Fi SSID/password, save, and restart the collector. Use a separate phone/computer if your current Wi-Fi adapter cannot stay on your normal network while provisioning.

After joining Wi-Fi, the serial console prints the dashboard's LAN IP. Open that IP in a browser. The setup AP switches off after the station connects and returns after a 30-second disconnection. Recovery AP access is by explicit IP; there is no captive portal. The ESP32 reconnects periodically when Wi-Fi is lost. Wi-Fi credentials and other secrets are not printed, apart from the admin password on the physical serial console.

Settings and credentials survive restarts. Empty secret fields preserve existing values. The printer form offers a separate clear-key checkbox. Wi-Fi/NTP/admin password changes need a restart for all network settings to take effect; the UI reports this. To recover a forgotten admin password, read the physical serial console at boot. To factory-reset, erase the board's flash and reflash; this deletes saved settings, collector ID, and credentials. Partition changes may require a one-time full erase; keep flash erasure disabled for ordinary firmware updates so NVS survives.

The local UI and Moonraker connection use HTTP on your trusted LAN. HTTP Basic authentication controls local access but does not encrypt it. Do not expose the ESP32, its setup AP, or Moonraker through public port-forwarding. NVS credentials are not hardware-encrypted by this package. Hardened commercial deployment would add secure boot/flash encryption and a more formal provisioning/recovery process.

## 3. Find and add printers

Discovery starts on Wi-Fi connection and repeats approximately every 15 minutes; **Discover printers** starts another scan. It checks the actual station subnet, not a hardcoded `192.168.1.x` range, on port **7125** by default. Set another scan port if your installation uses one.

- A successful `/server/info` response containing `moonraker_version` and `api_version` marks a verified service.
- A 401/403 is shown as an **unverified authentication-required candidate**; another HTTP server can also return 401. Add and authenticate it to confirm.
- Discovery displays candidates; clicking **Add** saves that printer and starts collection. It intentionally does not silently enroll every HTTP server in the network.
- The scanner does not broadcast mDNS and cannot discover across VLANs, guest isolation, routed subnets, or arbitrary ports. Large subnets with more than 1022 hosts are not scanned. A /24 scan can take a few minutes. The dashboard and collection use separate tasks while scanning.
- Manual addition supports other routable LAN addresses and ports. Use the Moonraker HTTP port, not the printer's proprietary app port. No HTTPS/reverse-proxy path prefix for printer-side Moonraker is implemented in this version.

If authentication is required, enter the printer's **Moonraker API key**. This is not the Linux SSH/root password. Username/password JWT login is not implemented; a saved API key avoids refresh-token expiry. Use Moonraker's existing authorized interface to obtain its key. Do not rotate an API key just to read it: rotation invalidates existing clients.

Save a DHCP reservation in the router for each printer. Prefer a stable DNS hostname if your router supports one. Every added printer receives a random persistent ID independent of its IP/model; when an address changes, **edit the existing printer** to preserve its VPS history. This version does not automatically bind a new IP to an old printer by hostname/model because neither is guaranteed unique. Deleting and re-adding creates a new ID. DNS hostnames resolve on each HTTP connection; `.local` resolution depends on the installed ESP32 core/network and is not guaranteed by this code.

## 4. Deploy the VPS service with Dokploy

A DNS hostname with a valid HTTPS certificate is recommended over a bare VPS IP. The ESP32 verifies both the server certificate chain and hostname. A bare IP works only if your certificate is valid for that IP.

1. Put the extracted **entire project** in a Git repository accessible to your Dokploy instance. Do not commit `.env` or credentials.
2. Create a **Docker Compose** service with this repository as its source and **`compose.yaml`** as the Compose path. Build context `./server` must remain available; pasting only the Compose YAML without the source files is insufficient.
3. Generate two separate random tokens, each at least 32 characters:

   ```bash
   python -c "import secrets; print(secrets.token_urlsafe(32))"
   ```

   Set `INGEST_TOKEN`, `DASHBOARD_TOKEN`, and optionally `RETENTION_DAYS=30` in Dokploy's environment settings. Run the command separately for each token. Each collector currently uses the shared ingest token; per-device token provisioning is not part of this version.
4. Deploy. Under Dokploy's **Domains** settings, route your hostname (for example `fleet.example.com`) to service **`printfleet`**, container port **`8000`**. Enable HTTPS and a valid certificate. Redeploy after domain/routing changes if Dokploy requests it.
5. Open `https://fleet.example.com/healthz`; expected response: `{"status":"ok"}`.
6. Open `https://fleet.example.com/` and enter the **dashboard token**. It remains only in that tab's JavaScript memory; reload/Lock requires it again.

Compose intentionally does not publish port 8000 on the VPS. Dokploy's proxy should route to the service. If running plain Docker Compose without Dokploy, supply an HTTPS reverse proxy and its network/routing configuration. The web service does not itself terminate TLS.

The named volume `printfleet_data` stores `/data/printfleet.db` and SQLite WAL files across redeployments. **Never delete this volume during an update.** The container uses UID 10001; an existing bind mount must be writable by that UID. Run one service replica and one uvicorn worker; this SQLite deployment is intended for a modest fleet. Larger fleets should move storage to PostgreSQL/time-series infrastructure and use per-collector identities.

### Backups and retention

Retention defaults to 30 days for samples and is checked at startup and hourly. The latest known state of each printer is retained separately even after historical samples expire; its age is shown so an old reading cannot look live. Row deletion reuses SQLite space; it does not automatically shrink the database file. Full raw samples can use several GB per day depending on fleet size, interval, and payload size: approximate raw volume = printers × 86400 / interval × mean serialized sample bytes, before indexes and SQLite overhead. Monitor free disk space and verify backups.

Use a consistent SQLite online backup, rather than copying just the main DB while WAL writes are active. For example, from the running service container:

```bash
python -c "import sqlite3; s=sqlite3.connect('/data/printfleet.db'); d=sqlite3.connect('/tmp/printfleet-backup.db'); s.backup(d); d.close(); s.close()"
```

Copy `/tmp/printfleet-backup.db` out of the container promptly and store it off-host. Restore into the persistent volume with the application stopped and correct ownership. Test restoring a backup before relying on it. Proxy-level request timeouts and rate limiting are appropriate for an internet-facing VPS; do not publish the ingestion token in URLs or frontend code.

## 5. Configure ESP32 → VPS

In the collector's **Settings**, enter:

| Setting | Example |
| --- | --- |
| VPS ingestion URL | `https://fleet.example.com/api/v1/telemetry` |
| VPS ingest token | The exact `INGEST_TOKEN` from Dokploy |
| TLS root CA | The trusted issuing **root** certificate in PEM format |
| NTP hostname | `pool.ntp.org`, or your reachable local NTP server |
| Poll interval | Start with 5 seconds; use 10–15 seconds for more printers |

Obtain the root certificate from the issuing CA's official repository, or export the root from your browser's verified certificate chain in Base-64/PEM format. Paste the `BEGIN CERTIFICATE` / `END CERTIFICATE` block. Do not paste a private key or use an arbitrary downloaded certificate. The server leaf certificate is not a durable root CA. Multiple PEM roots can be pasted within the 8,000-character setting limit for a planned CA transition. CA/root changes require updating this setting. The code never calls `setInsecure()`.

Network time must synchronize before HTTPS uploads. The ESP32 needs outbound DNS, NTP (normally UDP 123), HTTPS to your VPS, and LAN HTTP to the printers. Collection continues while waiting for NTP; samples without valid UTC use `captured_at: null`, and the VPS uses receipt time for those samples. `uptime_ms` and `boot_id` remain available.

## 6. Reliability behavior

| Event | Behavior |
| --- | --- |
| Printer offline / wrong port | Error sample, bounded HTTP timeouts, retry backoff up to 60 seconds |
| Wrong Moonraker key | Visible authentication error; edit the saved key |
| Klipper startup/shutdown/disconnected | Server state is recorded; object list is refreshed after Klipper is ready |
| Printer configuration changes | Runtime object inventory refreshed about every five minutes |
| VPS/network outage | Pending sample retried with jitter and exponential backoff up to 60 seconds |
| Ack lost after successful upload | Same boot ID and sequence retried; VPS accepts it once |
| Invalid auth/TLS/server error | Keep pending sample and retry; visible upload error/counters |
| Permanent payload/schema rejection (400/413/422) | Drop that sample, increment loss counter, proceed to later samples |
| RAM queue full | Keep older pending/queued samples, drop newly collected upload samples; local live state still updates |
| Reboot/power loss | All unacknowledged RAM samples lost; no flash telemetry writes; boot ID changes |
| VPS restarts | SQLite volume preserves history and latest state |

There are **12 queued messages plus up to one in-flight message**. They are global to the collector, not per printer. This buffer is short: at 8 printers / 5-second intervals it represents only around 8 seconds of production, depending on request duration. A prolonged outage drops samples by design. Counters are per boot and cannot count RAM samples lost during an unexpected power loss. A RAM-only solution cannot provide lossless durable delivery across power cuts; add external FRAM/SD or an always-on LAN relay if that becomes a requirement.

For samples from the same boot, `seq` prevents delayed retries from replacing newer live state. With valid UTC across boots, observation time orders samples. Without valid clocks across boots, arrival order is the documented fallback. The raw record includes collector ID, printer ID, boot ID, sequence, UTC when known, monotonic uptime, Moonraker eventtime, errors, raw objects, and diagnostics. Credentials are never part of telemetry.

The collector uses complete HTTP snapshots rather than a partial WebSocket merge cache. This costs more bandwidth but makes reconnect recovery straightforward. Local charts keep 180 recent points in browser RAM, while VPS charts aggregate persisted observations over 1 hour, 24 hours, or 7 days. Missing/offline readings are not converted to zero temperatures.

## 7. Bench verification

### Backend tests (no hardware needed)

```bash
python -m venv .venv
# Linux/macOS:
source .venv/bin/activate
# Windows PowerShell instead: .venv\Scripts\Activate.ps1
python -m pip install -r server/requirements.txt -r tests/requirements.txt
python -m pytest tests -q
```

### Local VPS service for development

```bash
# Set INGEST_TOKEN and DASHBOARD_TOKEN in your shell first (distinct, >=32 chars).
# Linux/macOS:
export DATABASE_PATH=./dev-data/printfleet.db
python -m uvicorn app:app --app-dir server --host 127.0.0.1 --port 8000
```

Open `http://127.0.0.1:8000` for the browser dashboard. Firmware uploads require HTTPS, so use your real Dokploy hostname or a properly configured development HTTPS proxy to test ESP32 delivery. Do not weaken certificate verification to test HTTP ingestion.

### Mock printer

```bash
pythontools/mock_moonraker.py --port 7125
# Optional authentication test:
pythontools/mock_moonraker.py --port 7125 --api-key test-key
```

Run it on a computer on your LAN, allow TCP 7125 in the computer firewall, and add that computer's LAN IP in the collector UI. The mock is clearly labelled `MOCK-Ender`; it is not a Creality compatibility certification.

### On the real printer and ESP32

1. Before flashing, verify from a computer on the same LAN:

   ```bash
   curl http://PRINTER_IP:7125/server/info
   curl -H "X-Api-Key: YOUR_KEY" http://PRINTER_IP:7125/printer/objects/list
   ```

2. Provision one printer and confirm its raw objects against the printer's own dashboard.
3. Start a small test print; check temperatures, state, filename, progress, and current-file metadata. The model field remains your manual label.
4. Add VPS settings; verify delivered count increases and the cloud dashboard receives the same printer ID.
5. Disconnect VPS access for two minutes; confirm retries and eventual loss counters. Restore access; verify the queue drains without duplicate records.
6. Disconnect/reconnect the printer, restart Klipper, rotate to an intentionally wrong test API key then restore it, and restart the router. Confirm recovery and clear error reporting.
7. Change a printer IP using **Edit**, not delete/re-add; verify history continuity.
8. Power-cycle the ESP32; verify credentials/settings remain and RAM-only loss behavior is understood.
9. Run 24–72 hours with the intended fleet, while watching free heap, queue, drop counters, real sampling intervals, temperatures, and VPS disk growth. Validate flash/NVS persistence with your exact Arduino board settings.

## Project layout

```text
firmware/PrintFleet/PrintFleet.ino  Arduino collector + HTTP configuration API
firmware/PrintFleet/web_ui.h       Generated embedded dashboard
firmware/PrintFleet/partitions.csv Enlarged NVS partition (configuration only)
server/app.py                     FastAPI, SQLite ingest/history, token authorization
server/static/index.html          Self-contained VPS dashboard
server/Dockerfile                 Non-root Python container
compose.yaml                      Dokploy Compose service + persistent volume
ui/dashboard.html                 Shared dashboard source
platformio.ini                    Reference Arduino/ESP32-S3 compile
tools/build_ui.py                Regenerates both dashboards
tools/mock_moonraker.py           LAN development printer fixture
tests/test_api.py                Backend integration tests
```

After editing the dashboard source, run `pythontools/build_ui.py` before compiling or deploying.

## Official API and deployment references

- Moonraker printer objects and query API: https://moonraker.readthedocs.io/en/latest/external_api/printer/
- Moonraker authorization: https://moonraker.readthedocs.io/en/latest/external_api/authorization/
- Klipper object/status reference: https://www.klipper3d.org/Status_Reference.html
- ESP32 partition tables: https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/partition_table.html
- Dokploy Compose domain routing: https://docs.dokploy.com/docs/core/docker-compose/domains
