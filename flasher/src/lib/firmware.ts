import { unzipSync } from "fflate";

export const REQUIRED_IMAGES = [
  { file: "bootloader.bin", address: 0x0 },
  { file: "partitions.bin", address: 0x8000 },
  { file: "ota_data_initial.bin", address: 0xf000 },
  { file: "firmware.bin", address: 0x20000 },
] as const;

export interface FirmwareImage {
  file: string;
  path: string;
  address: number;
  data: Uint8Array;
  sha256: string;
}

export interface FirmwareModel {
  id: string;
  label: string;
  images: FirmwareImage[];
  bytes: number;
}

function normalizedPath(path: string): string {
  return path.replace(/\\/g, "/").replace(/^\.\//, "").replace(/^\/+/, "");
}

export function parseChecksumFile(contents: string): Map<string, string> {
  const checksums = new Map<string, string>();
  for (const rawLine of contents.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line) continue;
    const match = line.match(/^([a-fA-F0-9]{64})\s+[*]?(.+)$/);
    if (!match) throw new Error(`Invalid checksum line: ${rawLine}`);
    checksums.set(normalizedPath(match[2]), match[1].toLowerCase());
  }
  if (checksums.size === 0) throw new Error("The checksum file is empty.");
  return checksums;
}

export async function sha256(data: Uint8Array): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", data as BufferSource);
  return Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, "0")).join("");
}

function modelLabel(id: string): string {
  return id
    .split(/[-_]/)
    .filter(Boolean)
    .map((part) => part.toUpperCase())
    .join(" ");
}

export async function prepareFirmwareArchive(
  archiveBytes: Uint8Array,
  checksumContents: string,
): Promise<FirmwareModel[]> {
  let archive: Record<string, Uint8Array>;
  try {
    archive = unzipSync(archiveBytes);
  } catch (error) {
    throw new Error(`Could not open the firmware ZIP: ${error instanceof Error ? error.message : String(error)}`);
  }

  const entries = new Map(
    Object.entries(archive)
      .filter(([path]) => !path.endsWith("/"))
      .map(([path, data]) => [normalizedPath(path), data]),
  );
  const checksums = parseChecksumFile(checksumContents);
  const modelIds = new Set<string>();

  for (const path of entries.keys()) {
    const match = path.match(/^out\/([^/]+)\/([^/]+)$/);
    if (match && REQUIRED_IMAGES.some((image) => image.file === match[2])) modelIds.add(match[1]);
  }

  const models: FirmwareModel[] = [];
  for (const id of [...modelIds].sort()) {
    const images: FirmwareImage[] = [];
    let incomplete = false;
    for (const required of REQUIRED_IMAGES) {
      const path = `out/${id}/${required.file}`;
      const data = entries.get(path);
      if (!data) {
        incomplete = true;
        break;
      }
      const expected = checksums.get(path);
      if (!expected) throw new Error(`No checksum was published for ${path}.`);
      const actual = await sha256(data);
      if (actual !== expected) throw new Error(`Checksum mismatch for ${path}. Download aborted.`);
      images.push({ ...required, path, data, sha256: actual });
    }
    // A partial folder may contain logs or interrupted build output; never offer it.
    if (!incomplete) {
      models.push({ id, label: modelLabel(id), images, bytes: images.reduce((sum, image) => sum + image.data.length, 0) });
    }
  }

  if (models.length === 0) {
    throw new Error(`No complete model was found. Each model needs ${REQUIRED_IMAGES.map((item) => item.file).join(", ")}.`);
  }
  return models;
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
}
