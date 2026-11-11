// 6c: the phone as a bridge carries the account's signed LoRa commands to
// the Gateway (same job as PC Edge's LoraBridge). The server signs and checks
// every answer; the Gateway only holds a command until its Tracker reports.
// Here: hand the commands over USB ("APP LORA QUEUE <hex>") and tell the
// server what the Gateway said. Nothing is decided on the phone.
// The Gateway holds one command per Tracker (a new one replaces the old), so
// only the oldest waiting command of each Tracker is handed; the next follows
// once the server has settled it.

/** One command: 1 frame for settings, 1-3 for an area list (resource 2). */
export interface LoraCommand { device: string; resource?: number; command_id: number; frames: string[]; }
export type LoraEvent =
  | { kind: "queued" | "sent" | "dropped"; device: string; command_id: number }
  | { kind: "result"; hex: string }
  /** The phone wrote the tasma over USB/Bluetooth: its state is at this revision now. */
  | { kind: "revision"; device: string; resource: number; revision: number };

export interface LoraServer {
  loraCommands(): Promise<LoraCommand[]>;
  loraEvents(events: LoraEvent[]): Promise<void>;
}

const QUEUED = /LORA QUEUED result=(\w+)/;
const SENT = /LORA SENT target=([0-9A-F]{16}) id=([0-9A-F]{8}) attempt=/;
const DROPPED = /LORA DROPPED target=([0-9A-F]{16}) id=([0-9A-F]{8})/;
const RESULT = /LORA RESULT .*\bhex=([0-9A-F]{112})\b/;
const FRAME_HEX = /^[0-9A-F]{108,510}$/;
/** One line stays short: USB line 200, Bluetooth Edge message 160. */
export const PIECE_HEX = 128;

/** A frame as "APP LORA + <piece>" ... "APP LORA QUEUE <last piece>". */
export function loraLines(frameHex: string): string[] {
  const pieces: string[] = [];
  for (let i = 0; i < frameHex.length; i += PIECE_HEX) pieces.push(frameHex.slice(i, i + PIECE_HEX));
  return [...pieces.slice(0, -1).map((p) => `APP LORA + ${p}`), `APP LORA QUEUE ${pieces[pieces.length - 1]}`];
}
export const CHECK_MS = 20_000;
export const REHAND_MS = 600_000;

export class LoraCarrier {
  private handed = new Map<string, { id: number; at: number }>();   // key: device
  private awaiting: { device: string; id: number }[] = [];
  private events: LoraEvent[] = [];
  private lastCheck = -Infinity;
  private busy = false;

  /** A (re)connected Gateway holds nothing: hand everything again. */
  reset(): void { this.handed.clear(); this.awaiting = []; }

  observe(line: string): void {
    let m = QUEUED.exec(line);
    if (m && this.awaiting.length) {
      const a = this.awaiting.shift()!;
      if (m[1] === "ADDED" || m[1] === "REPLACED") this.events.push({ kind: "queued", device: a.device, command_id: a.id });
      else this.forget(a.device);
      return;
    }
    if ((m = SENT.exec(line))) { this.events.push({ kind: "sent", device: m[1], command_id: parseInt(m[2], 16) }); return; }
    if ((m = DROPPED.exec(line))) {
      this.events.push({ kind: "dropped", device: m[1], command_id: parseInt(m[2], 16) });
      this.forget(m[1]);
      return;
    }
    if ((m = RESULT.exec(line))) this.events.push({ kind: "result", hex: m[1] });
  }

  private forget(device: string): void { this.handed.delete(device); }

  /** Report what was seen, then (every CHECK_MS) hand new commands over. */
  async poll(server: LoraServer | null, write: (line: string) => Promise<void>, now: number): Promise<void> {
    if (!server || this.busy) return;
    this.busy = true;
    try {
      if (this.events.length) {
        const batch = this.events.slice(0, 32);
        await server.loraEvents(batch);
        this.events = this.events.slice(batch.length);
      }
      if (now - this.lastCheck < CHECK_MS) return;
      this.lastCheck = now;
      const seen = new Set<string>();
      for (const c of await server.loraCommands()) {
        if (typeof c.device !== "string" || !Number.isInteger(c.command_id) || !Array.isArray(c.frames) ||
            c.frames.length < 1 || c.frames.length > 3 || !c.frames.every((f) => FRAME_HEX.test(f))) continue;
        if (seen.has(c.device)) continue;   // one per Tracker: the Gateway would replace the first
        seen.add(c.device);
        const key = c.device;
        const prev = this.handed.get(key);
        if (prev && prev.id === c.command_id && now - prev.at < REHAND_MS) continue;
        for (const f of c.frames) {
          for (const line of loraLines(f)) await write(line);
          this.awaiting.push({ device: c.device, id: c.command_id });
        }
        this.handed.set(key, { id: c.command_id, at: now });
      }
    } catch { /* offline: events stay, try again later */ } finally {
      this.busy = false;
    }
  }
}
