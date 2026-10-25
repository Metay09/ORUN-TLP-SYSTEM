// The app's one HTTP call: POST JSON with a bearer token. On Android,
// Capacitor's native HTTP (CapacitorHttp, enabled in capacitor.config.json)
// carries fetch, so the private-network backend is reachable from the app.
import type { HttpReply } from "../core/sync.ts";

export const HTTP_TIMEOUT_MS = 20000;

export async function fetchPost(url: string, token: string, body: string): Promise<HttpReply> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<never>((_, reject) => {
    timer = setTimeout(() => reject(new Error("sunucu cevap vermedi (20 sn)")), HTTP_TIMEOUT_MS);
  });
  try {
    const res = await Promise.race([
      fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}` },
        body,
      }),
      timeout,
    ]);
    let parsed: unknown = null;
    try { parsed = await res.json(); } catch { parsed = null; }
    return { status: res.status, body: parsed };
  } finally {
    clearTimeout(timer);
  }
}

export async function fetchGet(url: string, token: string): Promise<HttpReply> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<never>((_, reject) => {
    timer = setTimeout(() => reject(new Error("sunucu cevap vermedi (20 sn)")), HTTP_TIMEOUT_MS);
  });
  try {
    const res = await Promise.race([
      fetch(url, { headers: { Authorization: `Bearer ${token}` } }),
      timeout,
    ]);
    let parsed: unknown = null;
    try { parsed = await res.json(); } catch { parsed = null; }
    return { status: res.status, body: parsed };
  } finally {
    clearTimeout(timer);
  }
}
