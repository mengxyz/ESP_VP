import { zipSync } from "fflate";
import { describe, expect, it } from "vitest";
import { prepareFirmwareArchive, REQUIRED_IMAGES, sha256 } from "./firmware";

async function fixture(options?: { missing?: string; corruptChecksum?: boolean }) {
  const entries: Record<string, Uint8Array> = {};
  const checksums: string[] = [];
  for (const [index, image] of REQUIRED_IMAGES.entries()) {
    if (image.file === options?.missing) continue;
    const path = `out/p1s/${image.file}`;
    const data = new Uint8Array([index, 0x45, 0x53, 0x50, index + 1]);
    entries[path] = data;
    checksums.push(`${await sha256(data)}  ${path}`);
  }
  if (options?.corruptChecksum) checksums[0] = `${"0".repeat(64)}  out/p1s/bootloader.bin`;
  return { archive: zipSync(entries), checksums: checksums.join("\n") };
}

describe("prepareFirmwareArchive", () => {
  it("extracts a complete model and keeps the required flash layout", async () => {
    const input = await fixture();
    const models = await prepareFirmwareArchive(input.archive, input.checksums);

    expect(models).toHaveLength(1);
    expect(models[0].id).toBe("p1s");
    expect(models[0].images.map((image) => [image.file, image.address])).toEqual([
      ["bootloader.bin", 0x0],
      ["partitions.bin", 0x8000],
      ["ota_data_initial.bin", 0xf000],
      ["firmware.bin", 0x20000],
    ]);
  });

  it("blocks the archive when an image checksum does not match", async () => {
    const input = await fixture({ corruptChecksum: true });
    await expect(prepareFirmwareArchive(input.archive, input.checksums)).rejects.toThrow(
      "Checksum mismatch for out/p1s/bootloader.bin",
    );
  });

  it("does not offer incomplete model folders", async () => {
    const input = await fixture({ missing: "ota_data_initial.bin" });
    await expect(prepareFirmwareArchive(input.archive, input.checksums)).rejects.toThrow("No complete model was found");
  });
});
