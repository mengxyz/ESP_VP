export type Device = {
  device_id: string;
  name: string;
  ip?: string | null;
  management_url?: string | null;
  firmware_version?: string | null;
  configured: number | boolean;
  paired?: number | boolean;
  pair_ready?: number | boolean;
  pair_remaining_seconds?: number | null;
  claimed: number | boolean;
  receiver_managed: number | boolean;
  last_seen?: string | null;
};

export type DeviceEvent = {
  id: number;
  device_id: string;
  stage: string;
  status: "running" | "success" | "failure" | string;
  message: string;
  detail?: unknown;
  created_at: string;
};

export type ModelInfo = {
  display: string;
  product_name: string;
  serial_prefix: string;
};

export type Upload = {
  id: string;
  filename: string;
  bytes: number;
  source_ip?: string | null;
  vp_name?: string | null;
  device_id?: string | null;
  forward_mode: string;
  status: string;
  error?: string | null;
  created_at: string;
};

export type BambuddyPrinter = {
  id: number;
  name: string;
  serial_number?: string | null;
  ip_address?: string | null;
  model?: string | null;
  is_active?: boolean;
};

export type AppState = {
  version: string;
  first_run: boolean;
  settings: Record<string, unknown>;
  models: Record<string, ModelInfo>;
  ca_imported: boolean;
  devices: Device[];
  uploads: Upload[];
};

export type DeviceDetail = {
  device: Device;
  config: Record<string, unknown> | null;
  events: DeviceEvent[];
};

export class ApiError extends Error {
  status: number;
  payload: unknown;

  constructor(status: number, message: string, payload: unknown) {
    super(message);
    this.status = status;
    this.payload = payload;
  }
}
