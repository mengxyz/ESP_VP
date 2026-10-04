# ESP VP Web Flasher

A static Svelte app for installing published ESP VP firmware on an ESP32-S3 with Web Serial.

## Development

Requires Node.js 24+ and pnpm 10.

```bash
pnpm install
pnpm test
pnpm run check
pnpm run build
pnpm run dev
```

Web Serial requires desktop Chrome or Edge and a secure context. `localhost` is considered secure for local development.

The app reads public releases from `mengxyz/ESP_VP`. A usable release must contain assets named:

- `esp-vp-firmware-<tag>.zip`
- `SHA256SUMS-<tag>.txt`

Each model folder in the ZIP must provide `out/<model>/bootloader.bin`, `partitions.bin`, `ota_data_initial.bin`, and `firmware.bin`. Every image is SHA-256 verified before the Connect button is enabled.

GitHub's release blob host does not provide reliable browser CORS headers. The Pages workflow therefore runs `scripts/mirror-releases.mjs` after the Vite build and places the public release assets inside the same Pages artifact. The app still refreshes release metadata from the GitHub API and falls back to the deployed manifest if GitHub is rate-limited. For a fully functional local development picker, run `pnpm run mirror-releases` once; the generated `public/` directory is ignored.
