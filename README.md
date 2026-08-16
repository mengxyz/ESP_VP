# ESP32-S3 Virtual Printer Streaming Proxy

`esp-vp/` is a native ESP-IDF 6.0+ firmware project for an ESP32-S3 with PSRAM.
It presents one virtual Bambu printer on the LAN and streams slicer uploads to
the standalone receiver at:

```text
POST /api/v1/esp-vp/upload
```

The receiver then forwards the completed `.3mf` to stock Bambuddy's latest
library upload API:

```text
POST /api/v1/library/files
```

The firmware does not mount or write an SD card. FTP `STOR` data is relayed to
the receiver through a raw HTTP/1.1 chunked upload using fixed transfer buffers.

## Current Working Flow

The tested path is:

```text
Bambu Studio / OrcaSlicer
  -> discovers ESP VP on LAN
  -> connects to ESP bind/MQTT/FTPS services
  -> uploads .3mf over implicit FTPS
ESP VP
  -> streams FTP STOR bytes as HTTP chunked upload
  -> buddy_recv POST /api/v1/esp-vp/upload
buddy_recv
  -> writes a temporary .3mf
  -> forwards to stock Bambuddy POST /api/v1/library/files
Bambuddy
  -> stores it in Library
```

Latest stock Bambuddy still exposes `POST /api/v1/archives/upload`, but recent
versions can reject API keys there with `API keys cannot be used for
administrative operations`. For stock images, keep the receiver in `library`
mode. Archive and custom ESP ingest modes are compatibility options only.

TLS is capped to TLS 1.2 in the firmware to match the working Bambuddy virtual
printer behavior used by current slicers.

## Configuration

Normally you do not edit code. Run `build.py`; it writes
`include/app_config.generated.h` with:

- `APP_WIFI_SSID`, `APP_WIFI_PASSWORD`
- `APP_BAMBUDDY_BASE_URL`
- `APP_BAMBUDDY_API_KEY`
- `APP_VP_NAME`
- `APP_VP_MODEL_CODE`
- `APP_VP_ACCESS_CODE`
- `APP_VP_SERIAL_SUFFIX`

The default target is `esp32s3`. `sdkconfig.defaults` is configured for an
8 MB flash / 8 MB octal PSRAM ESP32-S3 module and the custom partition table in
`partitions.csv`.

## ESP-IDF 6 Setup

The build helper can use a project-local ESP-IDF checkout at:

```text
.tools/esp-idf
```

If `idf.py` is not available through that checkout, `build.py` will bootstrap it
inside the project:

1. Clone ESP-IDF from `https://github.com/espressif/esp-idf.git`
2. Use the default tag/branch `v6.0`
3. Run ESP-IDF's `install.sh esp32s3`
4. Run builds through `export.sh` automatically

This does not install ESP-IDF globally. Tool downloads still use Espressif's
normal installer cache, usually under `~/.espressif`.

You can still install/export ESP-IDF yourself if preferred:

```bash
source ~/esp/esp-idf/export.sh
idf.py --version
```

The project intentionally fails CMake configuration when `IDF_VERSION_MAJOR` is
less than 6.

## Build

Use the Python wrapper so model, serial suffix, password, API key, and shared
TLS cert/key are injected into a generated header. With no arguments it opens an
interactive terminal form:

```bash
python3 esp-vp/build.py
```

After a successful build it copies the esptool flash artifacts to:

```text
esp-vp/out/<model>/bootloader.bin
esp-vp/out/<model>/partitions.bin
esp-vp/out/<model>/firmware.bin
esp-vp/out/<model>/printer.crt
esp-vp/out/<model>/printer.key
esp-vp/out/<model>/bbl_ca.crt
esp-vp/out/<model>/virtual_printer_ca.crt
```

`printer.crt` and `printer.key` are the same TLS identity embedded into the
firmware. Import/copy `bbl_ca.crt` to Bambu Studio / OrcaSlicer when a trusted
printer certificate is needed; `virtual_printer_ca.crt` is the same CA with a
descriptive name. Keep `printer.key` private.

The flags are still available for repeatable builds:

```bash
python3 esp-vp/build.py \
  --model P1S,A1 \
  --wifi-ssid "Your WiFi" \
  --wifi-password "YourPassword" \
  --bambuddy-url "http://192.168.1.127:8000" \
  --api-key "bb_xxx"
```

ESP-IDF bootstrap flags:

```bash
python3 esp-vp/build.py --model X2D \
  --idf-path .tools/esp-idf \
  --idf-version v6.0
```

Use `--no-idf-bootstrap` when you want the build to fail instead of cloning or
installing SDK tools automatically.

If `--model` is omitted, the script shows a multi-model picker in interactive
terminals. You can select comma-separated indexes/codes/names, or `all`.
`--model` can also be repeated. The default access code is `12345678`, the
default Bambuddy URL is `http://192.168.1.127:8000`, and the default serial
suffix is fixed at `391800002`.

For multi-model builds, one serial suffix and one generated TLS cert/key are
shared across every selected firmware. Each model still receives its
model-specific serial prefix. For example, `--serial-suffix 123456789` produces
`01P00A123456789` for P1S and `03900A123456789` for A1. The generated header is
`include/app_config.generated.h` and is gitignored.

When rebuilding, the wrapper reuses an existing `out/<model>/printer.crt` and
`out/<model>/printer.key` from the selected models so Bambu Studio / OrcaSlicer
does not need a new trusted certificate after every firmware build. Pass
`--regen-cert` to intentionally rotate the certificate.

To reuse the same trusted CA as a hosted Bambuddy virtual printer, pass that
VP's shared CA files. The ESP build will create a new serial-specific printer
certificate signed by the imported CA:

```bash
python3 esp-vp/build.py --model X2D \
  --ca-cert /path/to/bbl_ca.crt \
  --ca-key /path/to/bbl_ca.key
```

You can run ESP-IDF directly after generating the header:

```bash
python3 esp-vp/build.py --model X2D --no-build
cd esp-vp
idf.py -B build/x2d -D SDKCONFIG=build/x2d/sdkconfig \
  -D SDKCONFIG_DEFAULTS=sdkconfig.defaults set-target esp32s3 build
```

Flash one generated model with esptool:

```bash
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 460800 \
  write_flash -z \
  0x0 esp-vp/out/p1s/bootloader.bin \
  0x8000 esp-vp/out/p1s/partitions.bin \
  0xf000 esp-vp/out/p1s/ota_data_initial.bin \
  0x20000 esp-vp/out/p1s/firmware.bin
```

Use the exact flash command printed by `build.py` when in doubt. Current
manager-mode builds use an OTA partition table, so the app image belongs at
`0x20000`, not `0x10000`.

After boot, check whether the ESP VP is reachable:

```bash
python3 esp-vp/check_ready.py 192.168.1.60
```

The checker probes TCP/3000, TCP/3002, TCP/8883, TCP/990, and sends a UDP/2021
discovery packet.

## Standalone Receiver

Run the standalone receiver beside Bambuddy when you want the ESP to upload to a
small sidecar instead of directly to the main Bambuddy process. The ESP uploads
to this receiver, and the receiver forwards the completed `.3mf` to Bambuddy.

The default forwarding mode is `library`, which uses the stock latest Bambuddy
API:

```text
POST /api/v1/library/files
```

This avoids a custom Bambuddy image and works with API keys. The older archive
upload endpoint, `POST /api/v1/archives/upload`, can reject API keys on recent
Bambuddy versions with `API keys cannot be used for administrative operations`,
so it is no longer the default.

If you want uploads to go directly into Archive instead of Library on latest
stock Bambuddy, use `--forward-mode archive` with a real user bearer token
instead of an API key. Latest Bambuddy treats archive upload as a user/admin
operation and can reject `bb_...` API keys there.

`buddy_recv.py` uses only the Python standard library.

Use this when:

- Bambuddy is already running and you do not want to rebuild its Docker image.
- The ESP firmware is already working, but you want a stable sidecar port in
  front of Bambuddy.
- The main Bambuddy host may be older and may not have the ESP ingest route.
- You want to keep the ESP target URL stable on another local port, usually
  `http://<host-ip>:8001`.

Start Bambuddy normally on port 8000, then run:

```bash
python3 esp-vp/buddy_recv.py \
  --host 0.0.0.0 \
  --port 8001 \
  --bambuddy-url http://127.0.0.1:8000 \
  --forward-mode library
```

If Bambuddy auth is enabled:

```bash
python3 esp-vp/buddy_recv.py \
  --host 0.0.0.0 \
  --port 8001 \
  --bambuddy-url http://127.0.0.1:8000 \
  --api-key bb_xxx
```

Archive mode with a user JWT:

```bash
python3 esp-vp/buddy_recv.py \
  --host 0.0.0.0 \
  --port 8001 \
  --bambuddy-url http://127.0.0.1:8000 \
  --forward-mode archive \
  --bearer-token "eyJ..."
```

Archive mode with automatic login:

```bash
python3 esp-vp/buddy_recv.py \
  --host 0.0.0.0 \
  --port 8001 \
  --bambuddy-url http://127.0.0.1:8000 \
  --forward-mode archive \
  --username admin \
  --password "your-password"
```

The receiver logs in through `POST /api/v1/auth/login`, caches the returned
JWT, and retries once with a fresh login if Bambuddy returns `401`. If the user
has 2FA enabled, Bambuddy does not return an access token from `/auth/login`;
use `--bearer-token` or a dedicated non-2FA service user.

Build manager-mode ESP firmware with the receiver URL, not the Bambuddy URL:

```bash
python3 esp-vp/build.py \
  --model X2D \
  --manager-mode \
  --receiver-url http://192.168.1.127:8001
```

Then flash the generated `out/<model>/firmware.bin`. When a slicer sends a file,
the ESP logs should show `stream_upload: uploaded ... status=200`, and the
receiver logs should show a successful forward to Bambuddy.

Health check:

```bash
curl http://127.0.0.1:8001/health
```

## VP Manager Pairing Guide

Use this flow for normal devices. The ESP should be a network bootstrap device
first; printer name, model, serial, access code, upload URL, certificate/key,
mode, paired printer, and LED brightness are configured later from VP Manager.
You should not need to rebuild firmware just to change the virtual printer.

The topology is:

```text
Bambuddy <-> VP Manager / buddy_recv <-> ESP VP <-> Bambu Studio / OrcaSlicer
```

Important URLs:

- `Bambuddy URL`: where VP Manager forwards uploads, for example
  `http://192.168.1.140:8000`.
- `Receiver URL for ESP uploads`: where the ESP sends uploads and receives
  config, for example `http://192.168.1.127:18081`.
- The ESP does not need to call Bambuddy directly in normal manager mode.

Start VP Manager:

```bash
python3 esp-vp/buddy_recv.py \
  --host 0.0.0.0 \
  --port 18081 \
  --bambuddy-url http://192.168.1.140:8000 \
  --receiver-url http://192.168.1.127:18081
```

Open the UI:

```text
http://127.0.0.1:18081
```

In `Settings`:

1. Set `Receiver URL for ESP uploads` to the LAN URL reachable from the ESP,
   usually `http://<manager-lan-ip>:18081`.
2. Set `Bambuddy URL` to the Bambuddy server URL.
3. Choose forwarding mode:
   - `library`: recommended default; API key is usually enough.
   - `archive`: uploads directly to Archives; requires Bambuddy username/password
     or a user/admin bearer token. API keys can be rejected by Bambuddy archive
     routes.
   - `proxy_status`: pair this ESP with a Bambuddy printer and let the ESP cache
     printer status for slicer report calls.
4. Click `Test Host`. The result shows both Bambuddy host reachability and
   archive-capable user auth.

Build a blank manager-mode firmware. Use the receiver URL, not the Bambuddy URL:

```bash
python3 esp-vp/build.py \
  --model P1S \
  --manager-mode \
  --status-led-pin 1 \
  --receiver-url http://192.168.1.127:18081 \
  --firmware-version 0005 \
  --wifi-ssid "Your WiFi" \
  --wifi-password "YourPassword"
```

Flash the full generated image set:

```bash
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 460800 \
  write_flash -z \
  0x0 esp-vp/out/p1s/bootloader.bin \
  0x8000 esp-vp/out/p1s/partitions.bin \
  0xf000 esp-vp/out/p1s/ota_data_initial.bin \
  0x20000 esp-vp/out/p1s/firmware.bin
```

First boot should show logs like:

```text
management: listening TCP/8080
esp_vp: device_info firmware=0005 manager_mode=1 configured=0 paired=0 ...
```

Pair the ESP:

1. Hold the ESP BOOT button for about 5 seconds.
2. The status LED enters pair mode for 120 seconds.
3. In VP Manager, open `Devices`, then `Add device`.
4. Scan/discover devices.
5. Select the discovered ESP and click `Pair`.

Pairing exchanges a receiver token. After pairing, VP Manager can authenticate
to the ESP management API on port `8080`. If the UI says `Pair the device before
pushing config`, hold BOOT again and pair the ESP; deleting a stale paired
device in the UI is safe and lets you pair it again.

Configure the virtual printer:

1. Open the paired device card.
2. Set name, model, serial, access code, mode, paired printer, and LED
   brightness.
3. Check `Generate and include printer cert/key` when you want VP Manager to
   generate and push the printer TLS identity.
4. Click `Save + Push`.

Successful push should show ESP logs like:

```text
device_config: applied config name="ESP VP X2D" model=N6 product="X2D" ...
esp_vp: printer services started
```

After config is pushed, the slicer should discover the configured name/model and
connect to the ESP on the normal printer ports:

```text
TCP/3000  bind
TCP/3002  bind TLS
TCP/8883  MQTT TLS
TCP/990   FTPS
TCP/8080  ESP management API for VP Manager only
```

Use OTA after the first full flash:

1. Build new firmware with a higher `--firmware-version`.
2. In the device card, click `Update firmware`.
3. Upload `esp-vp/out/<model>/firmware.bin` only.

Do not upload `bootloader.bin`, `partitions.bin`, or `ota_data_initial.bin` to
the OTA dialog. Those are only for USB flashing.

Troubleshooting:

- `ESP management API unavailable`: firmware is not listening on TCP/8080, the
  ESP IP changed, or the device is on another network.
- `HTTP 401` when pushing config or OTA: the ESP was reflashed or unpaired; pair
  it again with BOOT hold.
- `configured=0 paired=0`: expected for blank firmware before pairing/config.
- `configured=1 paired=1`: ESP has saved manager config and should survive
  reboot.
- Archive upload fails with Bambuddy login `401`: the Bambuddy username/password
  in VP Manager is wrong, uses an unsupported auth flow, or needs 2FA. Use a
  valid local service user or a current user/admin bearer token.

Docker option:

```bash
cd esp-vp
docker compose up -d --build
```

The compose file uses host networking. By default the container listens on host
port `8001` and forwards to `http://127.0.0.1:8000`. Override it if your
Bambuddy host API is elsewhere:

```bash
BAMBUDDY_URL=http://192.168.1.127:8000 \
docker compose up -d --build
```

With auth:

```bash
BAMBUDDY_API_KEY=bb_xxx \
docker compose up -d --build
```

Archive mode with a user JWT:

```bash
BUDDY_RECV_FORWARD_MODE=archive \
BAMBUDDY_BEARER_TOKEN="eyJ..." \
docker compose up -d --build
```

Archive mode with automatic login:

```bash
BUDDY_RECV_FORWARD_MODE=archive \
BAMBUDDY_USERNAME=admin \
BAMBUDDY_PASSWORD="your-password" \
docker compose up -d --build
```

With host networking, the receiver binds directly on the host. On Linux this is
usually the simplest mode because `127.0.0.1:8000` inside the container points to
the host network namespace. On Docker Desktop for macOS/Windows, host networking
support depends on the Docker version/settings; if `127.0.0.1:8000` does not
reach Bambuddy, set `BAMBUDDY_URL` to the host LAN IP instead.

Useful receiver environment variables:

```text
BUDDY_RECV_PORT=8001
BAMBUDDY_URL=http://127.0.0.1:8000
BAMBUDDY_API_KEY=
BAMBUDDY_BEARER_TOKEN=
BAMBUDDY_USERNAME=
BAMBUDDY_PASSWORD=
BUDDY_RECV_MAX_UPLOAD_BYTES=0
BUDDY_RECV_PRINTER_ID=
BUDDY_RECV_LIBRARY_FOLDER_ID=
BUDDY_RECV_FORWARD_MODE=library
```

Forward modes:

```text
library  Upload to stock latest /api/v1/library/files. This is the default.
auto     Try /api/v1/library/files, then fall back to /api/v1/archives/upload
         only when no API key is configured.
archive  Force the older /api/v1/archives/upload multipart API.
         On latest stock Bambuddy with auth enabled, use BAMBUDDY_BEARER_TOKEN
         instead of BAMBUDDY_API_KEY.
esp-vp   Require the custom /api/v1/esp-vp/upload route.
```

## Protocol Surface

- SSDP responder on UDP 2021 with Bambu discovery headers.
- Bind/detect listeners on TCP 3000 and TLS-capable TCP 3002.
- Minimal MQTT-over-TLS listener on 8883 that accepts slicer sessions and
  publishes archive-ready idle status.
- Implicit FTPS listener on 990 with one passive TLS data port, 50000.
- FTP `STOR` opens a streaming HTTP upload to Bambuddy with:
  - `X-Bambuddy-Filename`
  - `X-Bambuddy-VP-Name`
  - `X-Bambuddy-Source-IP`
  - `X-API-Key` when configured

The v1 scope is archive-only. It intentionally omits live-printer proxying,
camera, AMS, and print control.
