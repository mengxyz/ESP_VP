<script lang="ts">
  import { onMount } from "svelte";
  import { Check, ChevronRight, CircleAlert, Cpu, Download, Github, LoaderCircle, PlugZap, RefreshCw, ShieldCheck, Usb, Zap } from "lucide-svelte";
  import { ESPLoader, Transport } from "esptool-js";
  import { formatBytes, prepareFirmwareArchive, type FirmwareModel } from "./lib/firmware";
  import { downloadRelease, fetchReleases, type GithubRelease } from "./lib/releases";

  type Phase = "choose" | "ready" | "connected" | "flashing" | "done";

  let releases: GithubRelease[] = [];
  let selectedReleaseId = "";
  let models: FirmwareModel[] = [];
  let selectedModelId = "";
  let loadingReleases = true;
  let preparing = false;
  let phase: Phase = "choose";
  let error = "";
  let status = "Loading firmware releases…";
  let logs: string[] = [];
  let progress = 0;
  let connectedChip = "";
  let transport: Transport | null = null;
  let loader: ESPLoader | null = null;

  $: selectedRelease = releases.find((release) => String(release.id) === selectedReleaseId);
  $: selectedModel = models.find((model) => model.id === selectedModelId);
  $: serialAvailable = typeof navigator !== "undefined" && "serial" in navigator;
  $: secure = typeof window !== "undefined" && window.isSecureContext;
  $: supported = serialAvailable && secure;

  onMount(() => {
    const controller = new AbortController();
    void loadReleases(controller.signal);
    return () => {
      controller.abort();
      if (transport) void transport.disconnect().catch(() => undefined);
    };
  });

  function messageFrom(errorValue: unknown): string {
    if (errorValue instanceof DOMException && errorValue.name === "NotFoundError") return "No serial port was selected.";
    return errorValue instanceof Error ? errorValue.message : String(errorValue);
  }

  function addLog(message: string): void {
    const clean = message.trimEnd();
    if (clean) logs = [...logs.slice(-199), clean];
  }

  async function loadReleases(signal?: AbortSignal): Promise<void> {
    loadingReleases = true;
    error = "";
    status = "Loading firmware releases…";
    try {
      releases = await fetchReleases(signal);
      if (!releases.length) throw new Error("No published release has a complete firmware ZIP and checksum file yet.");
      selectedReleaseId = String(releases[0].id);
      status = "Choose a release to download and verify.";
    } catch (cause) {
      if (cause instanceof DOMException && cause.name === "AbortError") return;
      error = messageFrom(cause);
      status = "Could not load releases.";
    } finally {
      loadingReleases = false;
    }
  }

  async function prepareRelease(): Promise<void> {
    if (!selectedRelease) return;
    preparing = true;
    error = "";
    models = [];
    selectedModelId = "";
    phase = "choose";
    logs = [];
    try {
      const downloaded = await downloadRelease(selectedRelease, (message) => {
        status = message;
        addLog(message);
      });
      models = await prepareFirmwareArchive(downloaded.archive, downloaded.checksums);
      selectedModelId = models[0].id;
      phase = "ready";
      status = `${models.length} model${models.length === 1 ? "" : "s"} verified and ready.`;
      addLog(`Verified ${models.length} complete model folder${models.length === 1 ? "" : "s"}.`);
    } catch (cause) {
      error = messageFrom(cause);
      status = "Firmware preparation failed.";
      addLog(`ERROR: ${error}`);
    } finally {
      preparing = false;
    }
  }

  function releaseChanged(): void {
    models = [];
    selectedModelId = "";
    phase = "choose";
    error = "";
    progress = 0;
    void disconnect(false);
  }

  async function connect(): Promise<void> {
    if (!supported || !navigator.serial) return;
    error = "";
    logs = [];
    progress = 0;
    status = "Waiting for a USB serial port…";
    try {
      const port = await navigator.serial.requestPort();
      transport = new Transport(port as never, false);
      loader = new ESPLoader({
        transport,
        baudrate: 460800,
        terminal: {
          clean: () => { logs = []; },
          writeLine: (line: string) => addLog(line),
          write: (line: string) => addLog(line),
        },
      });
      status = "Connecting to the bootloader…";
      addLog("Connecting at 460800 baud. If this stalls, hold BOOT and tap RESET once.");
      const chip = await loader.main();
      connectedChip = String(chip);
      if (!connectedChip.toLowerCase().includes("esp32-s3") && !connectedChip.toLowerCase().includes("esp32s3")) {
        throw new Error(`Connected chip is ${connectedChip}, but this firmware requires an ESP32-S3.`);
      }
      phase = "connected";
      status = `${connectedChip} connected.`;
      addLog(`Ready: ${connectedChip}`);
    } catch (cause) {
      error = `${messageFrom(cause)} If connection fails, hold BOOT, tap RESET, then try again.`;
      status = "Connection failed.";
      await disconnect(false);
    }
  }

  async function flash(): Promise<void> {
    if (!loader || !selectedModel || !selectedRelease) return;
    error = "";
    phase = "flashing";
    progress = 0;
    status = `Flashing ${selectedModel.label}…`;
    const offsets = selectedModel.images.map((image) => image.address);
    const completedBefore = selectedModel.images.map((_, index) =>
      selectedModel.images.slice(0, index).reduce((sum, image) => sum + image.data.length, 0),
    );
    try {
      addLog("Writing four verified images without erasing the whole flash…");
      await loader.writeFlash({
        fileArray: selectedModel.images.map((image) => ({ data: image.data, address: image.address })),
        flashSize: "8MB",
        flashMode: "dio",
        flashFreq: "80m",
        eraseAll: false,
        compress: true,
        reportProgress: (fileIndex: number, written: number, total: number) => {
          const done = completedBefore[fileIndex] + Math.min(written, total);
          progress = Math.min(100, Math.round((done / selectedModel.bytes) * 100));
          status = `Writing 0x${offsets[fileIndex].toString(16)} — ${Math.round((written / total) * 100)}%`;
        },
      });
      progress = 100;
      status = "Flash complete. Resetting the device…";
      addLog("Firmware verified by esptool. Performing a hard reset.");
      await loader.after("hard_reset");
      phase = "done";
      status = `${selectedRelease.tag_name} installed successfully.`;
      addLog("Done. You can unplug the USB cable after the device restarts.");
    } catch (cause) {
      phase = "connected";
      error = messageFrom(cause);
      status = "Flashing stopped.";
      addLog(`ERROR: ${error}`);
    }
  }

  async function disconnect(updateStatus = true): Promise<void> {
    if (transport) {
      try { await transport.disconnect(); } catch { /* Port may already be gone. */ }
    }
    transport = null;
    loader = null;
    connectedChip = "";
    if (updateStatus && models.length) {
      phase = "ready";
      status = "Device disconnected.";
    }
  }
</script>

<svelte:head>
  <title>ESP VP Web Flasher</title>
</svelte:head>

<div class="page-shell">
  <header class="topbar">
    <a class="brand" href="./" aria-label="ESP VP flasher home">
      <span class="brand-mark"><Zap size={18} strokeWidth={2.5} /></span>
      <span>ESP VP <b>Flasher</b></span>
    </a>
    <a class="github-link" href="https://github.com/mengxyz/ESP_VP" target="_blank" rel="noreferrer">
      <Github size={18} /> Source
    </a>
  </header>

  <main>
    <section class="hero">
      <div class="eyebrow"><span></span> USB INSTALLER</div>
      <h1>Give your ESP32-S3<br /><em>a virtual printer.</em></h1>
      <p>Install a verified ESP VP release directly from your browser. No command line, drivers, or local build required.</p>
      <div class="trust-row">
        <span><ShieldCheck size={16} /> SHA-256 verified</span>
        <span><Usb size={16} /> Web Serial</span>
        <span><Cpu size={16} /> ESP32-S3</span>
      </div>
    </section>

    {#if !supported}
      <section class="support-warning" aria-live="polite">
        <CircleAlert size={24} />
        <div>
          <strong>This browser cannot flash over USB.</strong>
          <p>Open this page over HTTPS in desktop Chrome or Microsoft Edge. Web Serial is not available in Firefox or Safari.</p>
        </div>
      </section>
    {/if}

    <section class="installer-card">
      <div class="steps" aria-label="Install progress">
        <div class:active={true} class:complete={models.length > 0}><i>{models.length ? "✓" : "1"}</i><span>Release</span></div>
        <b></b>
        <div class:active={models.length > 0} class:complete={phase === "connected" || phase === "flashing" || phase === "done"}><i>{phase === "connected" || phase === "flashing" || phase === "done" ? "✓" : "2"}</i><span>Device</span></div>
        <b></b>
        <div class:active={phase === "connected" || phase === "flashing" || phase === "done"} class:complete={phase === "done"}><i>{phase === "done" ? "✓" : "3"}</i><span>Install</span></div>
      </div>

      {#if phase === "done"}
        <div class="done-panel">
          <div class="success-icon"><Check size={36} strokeWidth={2.5} /></div>
          <p class="section-kicker">INSTALL COMPLETE</p>
          <h2>{selectedRelease?.tag_name} is ready</h2>
          <p>Your {selectedModel?.label} firmware was written and verified. The ESP32-S3 has been reset.</p>
          <ol>
            <li><span>1</span><div><b>Let it reboot</b><small>Wait a few seconds for the status LED or boot log.</small></div></li>
            <li><span>2</span><div><b>Open VP Manager</b><small>Pair the new device and finish Wi-Fi configuration.</small></div></li>
          </ol>
          <button class="secondary full" on:click={() => { phase = "ready"; progress = 0; void disconnect(false); }}>
            Flash another device
          </button>
        </div>
      {:else}
        <div class="card-body">
          <section class="control-section">
            <div class="section-heading">
              <span class="number">01</span>
              <div><h2>Choose firmware</h2><p>Official builds are loaded from GitHub Releases.</p></div>
            </div>

            <label for="release">Release</label>
            <div class="select-row">
              <select id="release" bind:value={selectedReleaseId} on:change={releaseChanged} disabled={loadingReleases || preparing || phase === "flashing"}>
                {#if loadingReleases}<option value="">Loading releases…</option>{/if}
                {#each releases as release}
                  <option value={String(release.id)}>{release.tag_name}{release.prerelease ? " — pre-release" : ""}</option>
                {/each}
              </select>
              <button class="icon-button" title="Reload releases" aria-label="Reload releases" disabled={loadingReleases || preparing} on:click={() => loadReleases()}>
                <RefreshCw size={18} class={loadingReleases ? "spin" : ""} />
              </button>
            </div>

            {#if selectedRelease && !models.length}
              <button class="primary" disabled={preparing} on:click={prepareRelease}>
                {#if preparing}<LoaderCircle class="spin" size={19} />{:else}<Download size={19} />{/if}
                {preparing ? "Downloading & verifying…" : "Download & verify release"}
              </button>
            {/if}

            {#if models.length}
              <label for="model">Printer model</label>
              <select id="model" bind:value={selectedModelId} disabled={phase === "flashing" || phase === "connected"}>
                {#each models as model}<option value={model.id}>{model.label} · {formatBytes(model.bytes)}</option>{/each}
              </select>
              <div class="verified"><ShieldCheck size={18} /><span><b>Integrity verified</b><small>All four images match the release SHA-256 checksums.</small></span></div>
            {/if}
          </section>

          <section class:muted={!models.length} class="control-section divided">
            <div class="section-heading">
              <span class="number">02</span>
              <div><h2>Connect & install</h2><p>Use a data-capable USB cable connected directly to this computer.</p></div>
            </div>

            {#if phase === "connected" || phase === "flashing"}
              <div class="device-chip"><span class="pulse"></span><div><b>{connectedChip}</b><small>Connected at 460800 baud</small></div></div>
            {:else}
              <div class="boot-tip"><PlugZap size={21} /><span><b>Bootloader tip</b><small>If connect fails, hold BOOT, tap RESET once, then release BOOT.</small></span></div>
            {/if}

            {#if phase === "ready"}
              <button class="primary" disabled={!supported} on:click={connect}><Usb size={19} /> Connect ESP32-S3</button>
            {:else if phase === "connected"}
              <button class="primary danger-safe" on:click={flash}><Zap size={19} /> Flash {selectedModel?.label}<ChevronRight size={18} /></button>
            {:else if phase === "flashing"}
              <div class="progress-wrap">
                <div class="progress-label"><span>{status}</span><b>{progress}%</b></div>
                <div class="progress-track"><span style={`width: ${progress}%`}></span></div>
                <p>Keep the cable connected until flashing and verification finish.</p>
              </div>
            {/if}
          </section>

          {#if error}<div class="error-box" role="alert"><CircleAlert size={19} /><span>{error}</span></div>{/if}

          {#if logs.length}
            <details class="console" open={phase === "flashing" || !!error}>
              <summary><span>Status log</span><small>{status}</small></summary>
              <pre>{logs.join("\n")}</pre>
            </details>
          {/if}
        </div>
      {/if}
    </section>

    <p class="privacy"><ShieldCheck size={14} /> Firmware downloads come from GitHub. USB data stays between this browser and your device.</p>
  </main>
</div>
