import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";

const repository = process.env.GITHUB_REPOSITORY || "mengxyz/ESP_VP";
const outputDirectory = resolve(process.argv[2] || "public");
const headers = {
  Accept: "application/vnd.github+json",
  "X-GitHub-Api-Version": "2022-11-28",
  "User-Agent": "esp-vp-pages-builder",
};
if (process.env.GITHUB_TOKEN) headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`;

const response = await fetch(`https://api.github.com/repos/${repository}/releases?per_page=50`, { headers });
if (!response.ok) throw new Error(`GitHub releases request failed with HTTP ${response.status}`);

const releases = await response.json();
const mirrored = [];
await mkdir(outputDirectory, { recursive: true });

for (const release of releases) {
  if (release.draft) continue;
  const wantedNames = new Set([
    `esp-vp-firmware-${release.tag_name}.zip`,
    `SHA256SUMS-${release.tag_name}.txt`,
  ]);
  const wantedAssets = release.assets.filter((asset) => wantedNames.has(asset.name));
  if (wantedAssets.length !== 2) continue;

  const releaseDirectory = resolve(outputDirectory, "release-assets", String(release.id));
  await mkdir(releaseDirectory, { recursive: true });
  const localAssets = [];
  for (const asset of wantedAssets) {
    process.stdout.write(`Mirroring ${release.tag_name}/${asset.name}\n`);
    const assetResponse = await fetch(asset.browser_download_url, { headers });
    if (!assetResponse.ok) throw new Error(`${asset.name} download failed with HTTP ${assetResponse.status}`);
    await writeFile(resolve(releaseDirectory, asset.name), new Uint8Array(await assetResponse.arrayBuffer()));
    localAssets.push({
      name: asset.name,
      size: asset.size,
      browser_download_url: `./release-assets/${release.id}/${encodeURIComponent(asset.name)}`,
    });
  }
  mirrored.push({ ...release, assets: localAssets });
}

if (!mirrored.length) throw new Error("No release with both required firmware assets was found.");
await writeFile(resolve(outputDirectory, "release-assets.json"), `${JSON.stringify(mirrored)}\n`);
process.stdout.write(`Mirrored ${mirrored.length} firmware release(s).\n`);
