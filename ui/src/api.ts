import { ApiError } from "./types";

export async function api<T>(path: string, init: RequestInit = {}): Promise<T> {
  const headers = new Headers(init.headers);
  if (init.body && !(init.body instanceof FormData) && !headers.has("Content-Type")) {
    headers.set("Content-Type", "application/json");
  }

  const response = await fetch(path, { ...init, headers });
  const contentType = response.headers.get("content-type") ?? "";
  const payload = contentType.includes("json") ? await response.json() : await response.text();

  if (!response.ok) {
    const message =
      typeof payload === "object" && payload && "detail" in payload
        ? JSON.stringify((payload as { detail: unknown }).detail)
        : String(payload || response.statusText);
    throw new ApiError(response.status, message, payload);
  }

  return payload as T;
}

export function describeError(error: unknown): string {
  if (error instanceof ApiError) {
    const detail = typeof error.payload === "object" && error.payload && "detail" in error.payload ? (error.payload as { detail: unknown }).detail : error.payload;
    if (typeof detail === "object" && detail && "detail" in detail) {
      return String((detail as { detail: unknown }).detail);
    }
    return error.message;
  }
  return error instanceof Error ? error.message : String(error);
}
