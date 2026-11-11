// 6c: the phone as a bridge carries the account's signed LoRa commands to
// the Gateway (same job as PC Edge's LoraBridge). The server signs and checks
// every answer; the Gateway only holds a command until its Tracker reports.
// Here: hand the commands over USB ("APP LORA QUEUE <hex>") and tell the
// server what the Gateway said. Nothing is decided on the phone.

export interface LoraCommand { device: string; command_id: number; hex: string; }
export type LoraEvent =
  | { kind: "queued" | "sent" | "dropped"; device: string; command_id: number }
  | { kind: "result"; hex: string };

export interface LoraServer {
  loraCommands(): Promise<LoraCommand[]>;
  loraEvents(events: LoraEvent[]): Promise<void>;
}

const QUEUED = /LORA QUEUED result=(\w+)/;
const SENT = /LORA SENT target=([0-9A-F]{16}) id=([0-9A-F]{8}) attempt=/;
const DROPPED = /LORA DROPPED target=([0-9A-F]{16}) id=([0-9A-F]{8})/;
const RESULT = /LORA RESULT .*\bhex=([0-9A-F]{112})\b/;
const COMMAND_HEX = /^[0-9A-F]{108,178}$/;
export const CHECK_MS = 20_000;
export const REHAND_MS = 600_000;

export class LoraCarrier {
  private handed = new Map<string, { id: number; at: number }>();
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
      else this.handed.delete(a.device);
      return;
    }
    if ((m = SENT.exec(line))) { this.events.push({ kind: "sent", device: m[1], command_id: parseInt(m[2], 16) }); return; }
    if ((m = DROPPED.exec(line))) {
      this.events.push({ kind: "dropped", device: m[1], command_id: parseInt(m[2], 16) });
      this.handed.delete(m[1]);
      return;
    }
    if ((m = RESULT.exec(line))) this.events.push({ kind: "result", hex: m[1] });
  }

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
      for (const c of await server.loraCommands()) {
        if (typeof c.device !== "string" || !Number.isInteger(c.command_id) || !COMMAND_HEX.test(c.hex ?? "")) continue;
        const prev = this.handed.get(c.device);
        if (prev && prev.id === c.command_id && now - prev.at < REHAND_MS) continue;
        await write(`APP LORA QUEUE ${c.hex}`);
        this.awaiting.push({ device: c.device, id: c.command_id });
        this.handed.set(c.device, { id: c.command_id, at: now });
      }
    } catch { /* offline: events stay, try again later */ } finally {
      this.busy = false;
    }
  }
}
