import { describe, expect, it } from "vitest";
import { findFirmwareAssets, type GithubRelease } from "./releases";

const release: GithubRelease = {
  id: 1,
  tag_name: "v1.2.3",
  name: "ESP VP v1.2.3",
  published_at: "2026-10-04T00:00:00Z",
  prerelease: false,
  draft: false,
  html_url: "https://github.com/mengxyz/ESP_VP/releases/tag/v1.2.3",
  assets: [
    { name: "esp-vp-firmware-v1.2.3.zip", browser_download_url: "https://example.test/fw.zip", size: 100 },
    { name: "SHA256SUMS-v1.2.3.txt", browser_download_url: "https://example.test/sums.txt", size: 10 },
  ],
};

describe("findFirmwareAssets", () => {
  it("matches assets using the exact release tag", () => {
    expect(findFirmwareAssets(release)).toEqual({ archive: release.assets[0], checksums: release.assets[1] });
  });

  it("rejects a release missing either required asset", () => {
    expect(findFirmwareAssets({ ...release, assets: release.assets.slice(0, 1) })).toBeNull();
  });
});
