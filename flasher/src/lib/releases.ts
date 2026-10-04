export const RELEASES_API = "https://api.github.com/repos/mengxyz/ESP_VP/releases?per_page=50";

export interface ReleaseAsset {
  name: string;
  browser_download_url: string;
  size: number;
}

export interface GithubRelease {
  id: number;
  tag_name: string;
  name: string | null;
  published_at: string | null;
  prerelease: boolean;
  draft: boolean;
  html_url: string;
  assets: ReleaseAsset[];
}

export interface FirmwareAssets {
  archive: ReleaseAsset;
  checksums: ReleaseAsset;
}

export function findFirmwareAssets(release: GithubRelease): FirmwareAssets | null {
  const archiveName = `esp-vp-firmware-${release.tag_name}.zip`;
  const checksumName = `SHA256SUMS-${release.tag_name}.txt`;
  const archive = release.assets.find((asset) => asset.name === archiveName);
  const checksums = release.assets.find((asset) => asset.name === checksumName);
  return archive && checksums ? { archive, checksums } : null;
}

export async function fetchReleases(signal?: AbortSignal): Promise<GithubRelease[]> {
  const mirrorPromise = fetch(`${import.meta.env.BASE_URL}release-assets.json`, { signal })
    .then(async (response) => (response.ok && response.headers.get("content-type")?.includes("json")
      ? (await response.json()) as GithubRelease[]
      : []))
    .catch(() => [] as GithubRelease[]);

  try {
    const response = await fetch(RELEASES_API, {
      signal,
      headers: { Accept: "application/vnd.github+json" },
    });
    if (!response.ok) {
      const limit = response.headers.get("x-ratelimit-remaining");
      throw new Error(
        response.status === 403 && limit === "0"
          ? "GitHub's anonymous request limit was reached. Try again later."
          : `GitHub returned ${response.status} while loading releases.`,
      );
    }
    const live = ((await response.json()) as GithubRelease[])
      .filter((release) => !release.draft && findFirmwareAssets(release));
    const mirrored = await mirrorPromise;
    if (!mirrored.length) return live;

    // Only offer releases mirrored into this Pages deployment. This avoids
    // GitHub release-blob CORS restrictions and makes downloads same-origin.
    const liveById = new Map(live.map((release) => [release.id, release]));
    return mirrored.map((release) => ({ ...liveById.get(release.id), ...release }));
  } catch (error) {
    const mirrored = await mirrorPromise;
    if (mirrored.length) return mirrored;
    throw error;
  }
}

export async function downloadRelease(
  release: GithubRelease,
  onProgress?: (message: string) => void,
): Promise<{ archive: Uint8Array; checksums: string }> {
  const assets = findFirmwareAssets(release);
  if (!assets) throw new Error(`Release ${release.tag_name} does not contain the expected firmware assets.`);
  onProgress?.(`Downloading ${assets.archive.name}…`);
  const [archiveResponse, checksumResponse] = await Promise.all([
    fetch(assets.archive.browser_download_url),
    fetch(assets.checksums.browser_download_url),
  ]);
  if (!archiveResponse.ok) throw new Error(`Firmware download failed with HTTP ${archiveResponse.status}.`);
  if (!checksumResponse.ok) throw new Error(`Checksum download failed with HTTP ${checksumResponse.status}.`);
  const [archive, checksums] = await Promise.all([
    archiveResponse.arrayBuffer(),
    checksumResponse.text(),
  ]);
  onProgress?.("Verifying every firmware image…");
  return { archive: new Uint8Array(archive), checksums };
}
