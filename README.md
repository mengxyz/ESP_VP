# ESP32-S3 Virtual Printer Streaming Proxy

`esp-vp/` is a native ESP-IDF 6.0+ firmware project for an ESP32-S3 with PSRAM.
It presents one archive-only virtual Bambu printer on the LAN and streams slicer
uploads to Bambuddy at:

```text
POST /api/v1/esp-vp/upload
```

The firmware does not mount or write an SD card. FTP `STOR` data is relayed to
the backend through a raw HTTP/1.1 chunked upload using fixed transfer buffers.

## Current Working Flow

The tested path is:

```text
Bambu Studio / OrcaSlicer
  -> discovers ESP VP on LAN
  -> connects to ESP bind/MQTT/FTPS services
  -> uploads .3mf over implicit FTPS
ESP VP
  -> streams FTP STOR bytes as HTTP chunked upload
  -> POST /api/v1/esp-vp/upload
Bambuddy ingest
  -> writes a temporary .3mf
  -> archives it in Bambuddy
```

There are two supported ingest targets:

- **Built-in Bambuddy route**: use this when your Bambuddy checkout/image
  includes `POST /api/v1/esp-vp/upload`.
- **Standalone receiver**: use `buddy_recv.py` when you do not want to rebuild
  the main Bambuddy image. In that mode the path is:

```text
ESP -> buddy_recv -> existing Bambuddy host API
```

The receiver accepts the ESP upload, then forwards the completed `.3mf` to the
existing Bambuddy archive upload endpoint.

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

Install ESP-IDF 6.0 or newer, then export the environment before building:

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

If your running Bambuddy image does not include `POST /api/v1/esp-vp/upload`,
run the standalone receiver beside it instead of rebuilding the image. The ESP
uploads to this receiver, and the receiver forwards the completed `.3mf` to the
existing Bambuddy archive API (`POST /api/v1/archives/upload`).

`buddy_recv.py` uses only the Python standard library.

Use this when:

- Bambuddy is already running and you do not want to rebuild its Docker image.
- The ESP firmware is already working, but the main Bambuddy host does not have
  the ESP ingest route.
- You want to keep the ESP target URL stable on another local port, usually
  `http://<host-ip>:8001`.

Start Bambuddy normally on port 8000, then run:

```bash
python3 esp-vp/buddy_recv.py \
  --host 0.0.0.0 \
  --port 8001 \
  --bambuddy-url http://127.0.0.1:8000
```

If Bambuddy auth is enabled:

```bash
python3 esp-vp/buddy_recv.py \
  --host 0.0.0.0 \
  --port 8001 \
  --bambuddy-url http://127.0.0.1:8000 \
  --api-key bb_xxx
```

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
docker compose -f docker-compose.buddy-recv.yml up -d --build
```

The compose file uses host networking. By default the container listens on host
port `8001` and forwards to `http://127.0.0.1:8000`. Override it if your
Bambuddy host API is elsewhere:

```bash
BAMBUDDY_URL=http://192.168.1.127:8000 \
docker compose -f docker-compose.buddy-recv.yml up -d --build
```

With auth:

```bash
BAMBUDDY_API_KEY=bb_xxx \
docker compose -f docker-compose.buddy-recv.yml up -d --build
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
BUDDY_RECV_MAX_UPLOAD_BYTES=0
BUDDY_RECV_PRINTER_ID=
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
