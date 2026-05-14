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
  0x10000 esp-vp/out/p1s/firmware.bin
```

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

Build the ESP firmware with the receiver URL, not the Bambuddy URL:

```bash
python3 esp-vp/build.py --model X2D --bambuddy-url http://192.168.1.127:8001
```

Then flash the generated `out/<model>/firmware.bin`. When a slicer sends a file,
the ESP logs should show `stream_upload: uploaded ... status=200`, and the
receiver logs should show a successful forward to Bambuddy.

Health check:

```bash
curl http://127.0.0.1:8001/health
```

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
