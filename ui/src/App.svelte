<script lang="ts">
  import {
    Activity,
    Plus,
    CheckCircle2,
    CircleAlert,
    FileArchive,
    Gauge,
    KeyRound,
    Loader2,
    Radio,
    RefreshCw,
    Save,
    Settings,
    ShieldCheck,
    Trash2,
    UploadCloud,
    Wifi,
  } from "lucide-svelte";
  import { api, describeError } from "./api";
  import type { AppState, BambuddyPrinter, Device, DeviceDetail, DeviceEvent, ModelInfo, Upload } from "./types";

  const nav = [
    { id: "dashboard", label: "Dashboard", icon: Gauge },
    { id: "devices", label: "Devices", icon: Radio },
    { id: "settings", label: "Settings", icon: Settings },
    { id: "certificates", label: "Certificates", icon: ShieldCheck },
    { id: "uploads", label: "Uploads", icon: UploadCloud },
    { id: "logs", label: "Logs", icon: FileArchive },
  ];

  const emptyState: AppState = {
    version: "",
    first_run: false,
    settings: {},
    models: {},
    ca_imported: false,
    devices: [],
    uploads: [],
  };

  let state = emptyState;
  let route = "dashboard";
  let selectedDevice = "";
  let detail: DeviceDetail | null = null;
  let loading = true;
  let authMode: "setup" | "login" | null = null;
  let username = "";
  let password = "";
  let errorMessage = "";
  let busyAction = "";
  let lastResult = "";
  let toastTimer: ReturnType<typeof setTimeout> | null = null;
  let bambuddyTest: Record<string, unknown> | null = null;
  let bambuddyPrinters: BambuddyPrinter[] = [];
  let bambuddyPrintersError = "";
  let firmwareDialogOpen = false;
  let firmwareFile: File | null = null;
  let firmwareDragActive = false;
  let addDeviceDialogOpen = false;

  let settingsForm = {
    bambuddy_url: "",
    receiver_url: "",
    forward_mode: "library",
    api_key: "",
    bearer_token: "",
    username: "",
    password: "",
  };

  let deviceForm = {
    name: "",
    model_code: "C12",
    serial: "",
    access_code: "12345678",
    mode: "library",
    paired_printer_id: "",
    generate_cert: false,
  };

  let caCert = "";
  let caKey = "";

  function bool(value: unknown): boolean {
    return value === true || value === 1 || value === "1";
  }

  function parseRoute() {
    const parts = (window.location.hash.replace(/^#/, "") || "dashboard").split("/");
    route = nav.some((item) => item.id === parts[0]) ? parts[0] : "dashboard";
    selectedDevice = route === "devices" && parts[1] ? decodeURIComponent(parts[1]) : "";
  }

  function go(next: string) {
    window.location.hash = next;
  }

  async function load() {
    parseRoute();
    loading = true;
    errorMessage = "";
    try {
      state = await api<AppState>("/api/state");
      authMode = null;
      syncSettingsForm();
      if (selectedDevice) {
        await loadDevice(selectedDevice);
      } else {
        detail = null;
      }
    } catch (error) {
      if (error && typeof error === "object" && "status" in error) {
        const status = Number((error as { status: number }).status);
        authMode = status === 428 ? "setup" : status === 401 ? "login" : authMode;
      }
      if (!authMode) errorMessage = describeError(error);
    } finally {
      loading = false;
    }
  }

  async function loadDevice(deviceId: string, syncForm = true) {
    detail = await api<DeviceDetail>(`/api/devices/${encodeURIComponent(deviceId)}`);
    await loadBambuddyPrinters();
    if (!syncForm) return;
    const config = detail.config ?? {};
    deviceForm = {
      name: detail.device.name,
      model_code: String(config.model_code ?? "C12"),
      serial: String(config.serial ?? ""),
      access_code: String(config.access_code ?? "12345678"),
      mode: String(config.mode ?? state.settings.forward_mode ?? "library"),
      paired_printer_id: String(config.paired_printer_id ?? config.target_printer_id ?? config.printer_id ?? ""),
      generate_cert: state.ca_imported,
    };
    if (!deviceForm.serial) fillSerial(true);
  }

  async function loadBambuddyPrinters() {
    bambuddyPrintersError = "";
    try {
      bambuddyPrinters = await api<BambuddyPrinter[]>("/api/bambuddy/printers");
    } catch (error) {
      bambuddyPrinters = [];
      bambuddyPrintersError = describeError(error);
    }
  }

  function syncSettingsForm() {
    settingsForm = {
      bambuddy_url: String(state.settings.bambuddy_url ?? ""),
      receiver_url: String(state.settings.receiver_url ?? ""),
      forward_mode: String(state.settings.forward_mode ?? "library"),
      api_key: String(state.settings.api_key ?? ""),
      bearer_token: String(state.settings.bearer_token ?? ""),
      username: String(state.settings.username ?? ""),
      password: String(state.settings.password ?? ""),
    };
  }

  async function setupOrLogin() {
    errorMessage = "";
    try {
      await api(authMode === "setup" ? "/api/setup" : "/api/login", {
        method: "POST",
        body: JSON.stringify({ username, password }),
      });
      await load();
    } catch (error) {
      errorMessage = describeError(error);
    }
  }

  function submitAuth(event: SubmitEvent) {
    event.preventDefault();
    void setupOrLogin();
  }

  async function logout() {
    await api("/api/logout", { method: "POST" });
    window.location.reload();
  }

  async function discover() {
    await runAction("discover", async () => {
      const response = await api<{ devices: Device[]; receiver_url_sent?: string | null; receiver_url_header_sent?: boolean }>("/api/discover", { method: "POST" });
      await load();
      showToast(response.receiver_url_header_sent
        ? `Discovery sent receiver URL: ${response.receiver_url_sent}`
        : "Discovery ran, but no Receiver URL is configured in Settings");
    });
  }

  async function pairDevice(deviceId: string) {
    await runAction(`pair-${deviceId}`, async () => {
      const response = await api<Record<string, unknown>>(`/api/devices/${encodeURIComponent(deviceId)}/pair`, { method: "POST" });
      showToast(String(response.status ?? "Device paired"));
      addDeviceDialogOpen = false;
      await load();
    });
  }

  function pairedDevices(): Device[] {
    return state.devices.filter((device) => bool(device.paired ?? device.claimed));
  }

  function addableDevices(): Device[] {
    return state.devices.filter((device) => !bool(device.paired ?? device.claimed));
  }

  function openAddDeviceDialog() {
    addDeviceDialogOpen = true;
  }

  async function deleteDevice(deviceId: string) {
    if (!confirm("Delete this device from VP Manager? The ESP must be put into pair mode again before it can be managed.")) return;
    await runAction(`delete-${deviceId}`, async () => {
      await api(`/api/devices/${encodeURIComponent(deviceId)}`, { method: "DELETE" });
      if (selectedDevice === deviceId) {
        go("devices");
      }
      await load();
      showToast("Device deleted");
    });
  }

  async function saveSettings() {
    await runAction("settings", async () => {
      await api("/api/settings", { method: "POST", body: JSON.stringify(settingsForm) });
      await load();
      showToast("Settings saved");
    });
  }

  async function testBambuddyHost() {
    await runAction("bambuddy-test", async () => {
      bambuddyTest = await api<Record<string, unknown>>("/api/settings/test-bambuddy", {
        method: "POST",
        body: JSON.stringify({ bambuddy_url: settingsForm.bambuddy_url }),
      });
      showToast(
        bambuddyTest.status === "ok"
          ? `Bambuddy host reachable: HTTP ${String(bambuddyTest.status_code ?? "")}`
          : `Bambuddy host test: ${String(bambuddyTest.detail ?? bambuddyTest.status ?? "failed")}`);
    });
  }

  async function importCert() {
    await runAction("cert", async () => {
      await api("/api/certificates/ca", { method: "POST", body: JSON.stringify({ cert_pem: caCert, key_pem: caKey }) });
      caCert = "";
      caKey = "";
      await load();
      showToast("CA imported");
    });
  }

  async function saveDevice() {
    if (!selectedDevice) return;
    await runAction("save", async () => {
      await api(`/api/devices/${encodeURIComponent(selectedDevice)}/config`, { method: "POST", body: JSON.stringify(devicePayload()) });
      await loadDevice(selectedDevice);
      showToast("Configuration saved");
    });
  }

  async function probeDevice() {
    if (!selectedDevice) return;
    await runAction("probe", async () => {
      const response = await api<{ status: string }>(`/api/devices/${encodeURIComponent(selectedDevice)}/probe`, { method: "POST" });
      await loadDevice(selectedDevice);
      showToast(`Probe ${response.status}`);
    });
  }

  async function pushDevice(saveFirst = false) {
    if (!selectedDevice) return;
    await runAction(saveFirst ? "save-push" : "push", async () => {
      if (saveFirst) {
        await api(`/api/devices/${encodeURIComponent(selectedDevice)}/config`, { method: "POST", body: JSON.stringify(devicePayload()) });
      }
      const response = await api<Record<string, unknown>>(`/api/devices/${encodeURIComponent(selectedDevice)}/push-config`, { method: "POST" });
      await loadDevice(selectedDevice);
      showToast(String(response.status ?? "Config pushed"));
    });
  }

  function openFirmwareDialog() {
    firmwareFile = null;
    firmwareDragActive = false;
    firmwareDialogOpen = true;
  }

  function selectFirmwareFile(files: FileList | null) {
    firmwareFile = files && files.length > 0 ? files[0] : null;
  }

  function dropFirmware(event: DragEvent) {
    event.preventDefault();
    firmwareDragActive = false;
    selectFirmwareFile(event.dataTransfer?.files ?? null);
  }

  async function uploadFirmware() {
    if (!selectedDevice || !firmwareFile) return;
    await runAction("firmware", async () => {
      const form = new FormData();
      form.append("firmware", firmwareFile as Blob, firmwareFile?.name ?? "firmware.bin");
      const response = await api<Record<string, unknown>>(`/api/devices/${encodeURIComponent(selectedDevice)}/firmware`, {
        method: "POST",
        body: form,
      });
      firmwareDialogOpen = false;
      firmwareFile = null;
      await loadDevice(selectedDevice);
      showToast(String(response.status ?? "Firmware uploaded; ESP is rebooting"));
    });
  }

  async function runAction(name: string, fn: () => Promise<void>) {
    busyAction = name;
    errorMessage = "";
    lastResult = "";
    try {
      await fn();
    } catch (error) {
      errorMessage = describeError(error);
      if (selectedDevice) {
        await loadDevice(selectedDevice).catch(() => undefined);
      }
    } finally {
      busyAction = "";
    }
  }

  function showToast(message: string) {
    lastResult = message;
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
      lastResult = "";
      toastTimer = null;
    }, 4500);
  }

  function modelOptions(): [string, ModelInfo][] {
    return Object.entries(state.models);
  }

  function autoSerial(modelCode: string, deviceId: string): string {
    const model = state.models[modelCode] ?? state.models.C12 ?? { serial_prefix: "01P00A" };
    let hash = 0;
    for (const char of deviceId) hash = (hash * 31 + char.charCodeAt(0)) % 1_000_000_000;
    return `${model.serial_prefix}${String(hash).padStart(9, "0")}`;
  }

  function fillSerial(force = false) {
    if (!force && deviceForm.serial) return;
    deviceForm.serial = autoSerial(deviceForm.model_code, selectedDevice || "vp");
  }

  function devicePayload() {
    const model = state.models[deviceForm.model_code];
    return {
      name: deviceForm.name,
      model_code: deviceForm.model_code,
      product_name: model?.product_name ?? deviceForm.model_code,
      serial: deviceForm.serial,
      access_code: deviceForm.access_code,
      mode: deviceForm.mode,
      paired_printer_id: deviceForm.mode === "proxy_status" && deviceForm.paired_printer_id ? Number(deviceForm.paired_printer_id) : undefined,
      receiver_url: settingsForm.receiver_url || undefined,
      generate_cert: deviceForm.generate_cert,
    };
  }

  function formatStage(stage: string): string {
    return stage
      .replace(/_/g, " ")
      .replace(/\b\w/g, (char) => char.toUpperCase())
      .replace("Config Save", "Save");
  }

  function eventClass(event: DeviceEvent): string {
    if (event.status === "failure") return "danger";
    if (event.status === "running") return "working";
    return "success";
  }

  function latestProbe(events: DeviceEvent[] = []): DeviceEvent | undefined {
    return [...events].reverse().find((event) => event.stage === "probe");
  }

  function routeTitle() {
    if (route === "devices" && selectedDevice && detail) return detail.device.name;
    return nav.find((item) => item.id === route)?.label ?? "Dashboard";
  }

  window.addEventListener("hashchange", () => {
    void load();
  });

  if (!window.location.hash) window.location.hash = "#dashboard";
  void load();
</script>

{#if authMode}
  <main class="auth-shell">
    <form class="auth-card" on:submit={submitAuth}>
      <div class="auth-mark"><KeyRound size={24} /></div>
      <div>
        <h1>{authMode === "setup" ? "First-run setup" : "VP Manager Login"}</h1>
        <p class="muted">{authMode === "setup" ? "Create the admin account for this receiver." : "Sign in to manage paired ESP VP devices."}</p>
      </div>
      <label>
        Username
        <input bind:value={username} autocomplete="username" />
      </label>
      <label>
        Password
        <input bind:value={password} type="password" autocomplete={authMode === "setup" ? "new-password" : "current-password"} />
      </label>
      {#if errorMessage}<p class="error">{errorMessage}</p>{/if}
      <button class="button primary auth-submit" type="submit" disabled={!!busyAction || !username || !password}>
        {busyAction ? "Working..." : authMode === "setup" ? "Create Admin" : "Log In"}
      </button>
    </form>
  </main>
{:else}
  <div class="shell">
    <aside>
      <div class="brand">
        <Activity size={22} />
        <strong>ESP VP Manager</strong>
      </div>
      <nav>
        {#each nav as item}
          <button class:active={route === item.id} on:click={() => go(item.id)}>
            <svelte:component this={item.icon} size={18} />
            {item.label}
          </button>
        {/each}
      </nav>
    </aside>

    <main>
      <header>
        <div>
          <h1>{routeTitle()}</h1>
          <p>{state.devices.length} device(s) · Version {state.version || "unknown"}</p>
        </div>
        <div class="toolbar">
          <button class="button icon" title="Refresh" on:click={load}><RefreshCw size={17} /></button>
          <button class="button" on:click={logout}>Log out</button>
        </div>
      </header>

      {#if loading}
        <section class="center"><Loader2 class="spin" /> Loading</section>
      {:else}
        {#if errorMessage}
          <div class="notice danger"><CircleAlert size={18} /> {errorMessage}</div>
        {/if}
        {#if route === "dashboard"}
          <section class="grid">
            <article class="panel">
              <h2>Receiver</h2>
              <dl>
                <dt>Forward mode</dt><dd>{String(state.settings.forward_mode ?? "")}</dd>
                <dt>ESP receiver URL</dt><dd>{String(state.settings.receiver_url ?? "browser URL fallback")}</dd>
                <dt>Bambuddy</dt><dd>{String(state.settings.bambuddy_url ?? "")}</dd>
                <dt>CA imported</dt><dd>{state.ca_imported ? "Yes" : "No"}</dd>
              </dl>
            </article>
            <article class="panel">
              <h2>Enrollment Key</h2>
              <pre>{String(state.settings.enrollment_key ?? "")}</pre>
            </article>
          </section>
        {:else if route === "devices"}
          <section class="stack">
            <div class="section-bar">
              <h2>Devices</h2>
              <div class="toolbar">
                <button class="button primary" on:click={openAddDeviceDialog}>
                  <Plus size={16} /> Add Device
                </button>
              </div>
            </div>
            {#if pairedDevices().length}
              <div class="device-cards">
                {#each pairedDevices() as device}
                  <article class="device-card" class:active={selectedDevice === device.device_id}>
                    <div>
                      <h3>{device.name}</h3>
                      <code>{device.device_id}</code>
                    </div>
                    <div class="badges">
                      <span class="badge ok">paired</span>
                      <span class="badge">{bool(device.configured) ? "configured" : "needs config"}</span>
                      <span class="badge">FW {device.firmware_version ?? "unknown"}</span>
                    </div>
                    <dl>
                      <dt>IP</dt><dd>{device.ip ?? "unknown"}</dd>
                      <dt>Seen</dt><dd>{device.last_seen ?? "never"}</dd>
                    </dl>
                    <div class="button-row">
                      <button class="button primary" on:click={() => go(`devices/${encodeURIComponent(device.device_id)}`)}>Open</button>
                      <button class="button danger" on:click={() => deleteDevice(device.device_id)}><Trash2 size={15} /> Delete</button>
                    </div>
                  </article>
                {/each}
              </div>
            {:else}
              <section class="empty-panel">
                <Radio size={26} />
                <h2>No paired devices</h2>
                <p class="muted">Hold BOOT for 5 seconds on an ESP until it blinks cyan, then add it here.</p>
                <button class="button primary" on:click={openAddDeviceDialog}><Plus size={16} /> Add Device</button>
              </section>
            {/if}

            {#if selectedDevice && detail}
              {@const probe = latestProbe(detail.events)}
              <section class="device-layout">
                <article class="panel">
                  <div class="badges">
                    <span class="badge">{bool(detail.device.paired ?? detail.device.claimed) ? "Paired" : "Unpaired"}</span>
                    <span class="badge" class:ok={bool(detail.device.pair_ready)}>
                      {bool(detail.device.pair_ready) ? `Ready to pair ${detail.device.pair_remaining_seconds ?? ""}s` : "Not in pair mode"}
                    </span>
                    <span class="badge">{bool(detail.device.configured) ? "Configured" : "Needs config"}</span>
                    <span class="badge">{detail.device.last_seen ? `Seen ${detail.device.last_seen}` : "Not seen"}</span>
                    <span class="badge">Firmware {detail.device.firmware_version ?? "unknown"}</span>
                    <span class="badge">IP {detail.device.ip ?? "unknown"}</span>
                    <span class="badge" class:ok={probe?.status === "success"} class:bad={probe?.status === "failure"}>
                      API {probe?.status === "success" ? "reachable" : probe?.status === "failure" ? "unavailable" : "unknown"}
                    </span>
                  </div>
                  <div class="compact-form">
                    <label>Name<input bind:value={deviceForm.name} /></label>
                    <label>
                      Model
                      <select bind:value={deviceForm.model_code} on:change={() => fillSerial(false)}>
                        {#each modelOptions() as [code, model]}
                          <option value={code}>{model.display} ({code})</option>
                        {/each}
                      </select>
                    </label>
                    <label>
                      Serial
                      <div class="input-row">
                        <input bind:value={deviceForm.serial} />
                        <button class="button" on:click={() => fillSerial(true)}>Auto</button>
                      </div>
                    </label>
                    <label>Access code<input bind:value={deviceForm.access_code} maxlength="8" /></label>
                    <label>
                      Mode
                      <select bind:value={deviceForm.mode}>
                        <option>immediate</option><option>print_queue</option><option>library</option><option>proxy_status</option>
                      </select>
                    </label>
                  </div>
                  {#if deviceForm.mode === "proxy_status"}
                    <label class="wide-field">
                      Paired Bambuddy printer
                      <select bind:value={deviceForm.paired_printer_id}>
                        <option value="">Select printer</option>
                        {#each bambuddyPrinters as printer}
                          <option value={String(printer.id)}>
                            {printer.name} #{printer.id} {printer.model ? `(${printer.model})` : ""} {printer.ip_address ? `- ${printer.ip_address}` : ""}
                          </option>
                        {/each}
                      </select>
                    </label>
                    {#if bambuddyPrintersError}
                      <p class="error">Printer list failed: {bambuddyPrintersError}</p>
                    {:else}
                      <p class="muted">ESP will poll this Bambuddy printer status and report the cached state to slicers.</p>
                    {/if}
                  {/if}
                  <label class="check">
                    <input type="checkbox" bind:checked={deviceForm.generate_cert} disabled={!state.ca_imported} />
                    Generate and include printer cert/key
                  </label>
                  <div class="button-row">
                    <button class="button primary" disabled={!!busyAction} on:click={() => pushDevice(true)}><Save size={16} /> Save + Push</button>
                    <button class="button" disabled={!!busyAction} on:click={saveDevice}>Save</button>
                    <button class="button" disabled={!!busyAction} on:click={() => pairDevice(selectedDevice)}>Pair</button>
                    <button class="button" disabled={!!busyAction} on:click={probeDevice}><Wifi size={16} /> Probe</button>
                    <button class="button" disabled={!!busyAction} on:click={() => pushDevice(false)}>Push saved</button>
                    <button class="button" disabled={!!busyAction || !bool(detail.device.paired ?? detail.device.claimed)} on:click={openFirmwareDialog}><UploadCloud size={16} /> Update FW</button>
                    {#if bool(detail.device.paired ?? detail.device.claimed)}
                      <button class="button danger" disabled={!!busyAction} on:click={() => deleteDevice(selectedDevice)}><Trash2 size={16} /> Delete</button>
                    {/if}
                  </div>
                  <p class="muted">To pair: hold BOOT for 5 seconds until the RGB LED blinks cyan, then click Pair.</p>
                  <p class="muted">Management API: {detail.device.management_url ?? "not reported"}</p>
                </article>

                <article class="panel timeline-panel">
                  <h2>Timeline</h2>
                  <ol class="timeline">
                    {#each detail.events as event}
                      <li class={eventClass(event)}>
                        <span></span>
                        <div>
                          <strong>{formatStage(event.stage)}</strong>
                          <p>{event.message}</p>
                          <small>{event.created_at}</small>
                        </div>
                      </li>
                    {/each}
                  </ol>
                </article>
              </section>
            {/if}
          </section>
        {:else if route === "settings"}
          <section class="panel form-panel">
            <label>Receiver URL for ESP uploads<input bind:value={settingsForm.receiver_url} placeholder="http://192.168.1.127:18081" /></label>
            <label>Bambuddy URL<input bind:value={settingsForm.bambuddy_url} /></label>
            <div class="button-row">
              <button class="button" disabled={busyAction === "bambuddy-test"} on:click={testBambuddyHost}>
                <Wifi size={16} />
                {busyAction === "bambuddy-test" ? "Testing..." : "Test Host"}
              </button>
              {#if bambuddyTest}
                <span class="badge" class:ok={bambuddyTest.status === "ok"} class:bad={bambuddyTest.status !== "ok"}>
                  {String(bambuddyTest.status ?? "unknown")}
                </span>
              {/if}
            </div>
            {#if bambuddyTest}
              <pre>{JSON.stringify(bambuddyTest, null, 2)}</pre>
            {/if}
            <label>
              Forward mode
              <select bind:value={settingsForm.forward_mode}>
                <option>immediate</option><option>print_queue</option><option>library</option><option>proxy_status</option><option>archive</option><option>esp-vp</option><option>auto</option>
              </select>
            </label>
            <label>API key<input bind:value={settingsForm.api_key} /></label>
            <label>Bearer token<input bind:value={settingsForm.bearer_token} /></label>
            <label>Username<input bind:value={settingsForm.username} /></label>
            <label>Password<input bind:value={settingsForm.password} type="password" /></label>
            <button class="button primary" on:click={saveSettings}>Save Settings</button>
          </section>
        {:else if route === "certificates"}
          <section class="panel form-panel">
            <label>CA certificate PEM<textarea bind:value={caCert} rows="9"></textarea></label>
            <label>CA private key PEM<textarea bind:value={caKey} rows="9"></textarea></label>
            <button class="button primary" on:click={importCert}>Import CA</button>
          </section>
        {:else if route === "uploads"}
          <section class="table-wrap">
            <table>
              <thead><tr><th>Time</th><th>File</th><th>Bytes</th><th>Mode</th><th>Status</th></tr></thead>
              <tbody>
                {#each state.uploads as upload}
                  <tr>
                    <td>{upload.created_at}</td><td>{upload.filename}</td><td>{upload.bytes}</td><td>{upload.forward_mode}</td><td>{upload.status || upload.error}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </section>
        {:else if route === "logs"}
          <section class="panel"><pre>{JSON.stringify(state, null, 2)}</pre></section>
        {/if}
      {/if}
    </main>
  </div>
{/if}

{#if !authMode && lastResult}
  <div class="toast success" role="status" aria-live="polite">
    <CheckCircle2 size={18} /> {lastResult}
  </div>
{/if}

{#if addDeviceDialogOpen}
  <div class="modal-backdrop" role="presentation" on:click={() => (addDeviceDialogOpen = false)}>
    <div class="modal add-device-modal" role="dialog" aria-modal="true" aria-label="Add device" tabindex="-1" on:click|stopPropagation on:keydown|stopPropagation>
      <div class="section-bar">
        <div>
          <h2>Add Device</h2>
          <p class="muted">Hold BOOT for 5 seconds until the ESP blinks cyan, then scan and pair.</p>
        </div>
        <button class="button" disabled={busyAction === "discover"} on:click={discover}>
          <RefreshCw size={16} class={busyAction === "discover" ? "spin" : ""} /> Scan
        </button>
      </div>

      <div class="pair-list">
        {#if addableDevices().length}
          {#each addableDevices() as device}
            <article class="pair-row">
              <div>
                <strong>{device.name}</strong>
                <p><code>{device.device_id}</code> · {device.ip ?? "unknown IP"} · FW {device.firmware_version ?? "unknown"}</p>
              </div>
              <div class="badges">
                {#if bool(device.pair_ready)}
                  <span class="badge ok">ready {device.pair_remaining_seconds ?? ""}s</span>
                {:else}
                  <span class="badge">hold BOOT</span>
                {/if}
              </div>
              <button class="button primary" disabled={!!busyAction || !bool(device.pair_ready)} on:click={() => pairDevice(device.device_id)}>
                Pair
              </button>
            </article>
          {/each}
        {:else}
          <section class="empty-panel compact">
            <Radio size={24} />
            <h2>No unpaired ESPs found</h2>
            <p class="muted">Put the ESP in pair mode, then Scan.</p>
          </section>
        {/if}
      </div>

      <div class="button-row end">
        <button class="button" on:click={() => (addDeviceDialogOpen = false)}>Close</button>
      </div>
    </div>
  </div>
{/if}

{#if firmwareDialogOpen}
  <div class="modal-backdrop" role="presentation" on:click={() => (firmwareDialogOpen = false)}>
    <div class="modal" role="dialog" aria-modal="true" aria-label="Upload firmware" tabindex="-1" on:click|stopPropagation on:keydown|stopPropagation>
      <h2>Update Firmware</h2>
      <div
        class="dropzone"
        class:active={firmwareDragActive}
        role="button"
        tabindex="0"
        on:dragover|preventDefault={() => (firmwareDragActive = true)}
        on:dragleave={() => (firmwareDragActive = false)}
        on:drop={dropFirmware}
      >
        <UploadCloud size={26} />
        <strong>{firmwareFile ? firmwareFile.name : "Drop firmware .bin here"}</strong>
        <span>{firmwareFile ? `${firmwareFile.size} bytes` : "or choose a file"}</span>
        <input type="file" accept=".bin,application/octet-stream" on:change={(event) => selectFirmwareFile(event.currentTarget.files)} />
      </div>
      <div class="button-row">
        <button class="button primary" disabled={!firmwareFile || busyAction === "firmware"} on:click={uploadFirmware}>
          {busyAction === "firmware" ? "Uploading..." : "Upload and Reboot"}
        </button>
        <button class="button" disabled={busyAction === "firmware"} on:click={() => (firmwareDialogOpen = false)}>Cancel</button>
      </div>
      <p class="muted">Requires an OTA-capable firmware already flashed over USB once.</p>
    </div>
  </div>
{/if}
