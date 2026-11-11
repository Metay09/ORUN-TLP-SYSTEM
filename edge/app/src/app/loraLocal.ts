// 6c/6d: the phone signs LoRa commands itself when it bridges a gateway
// without internet (owner 2026-11-11). Same job as backend/lora_queue.py:
// every open settings/area operation for a tasma whose write key this phone
// holds becomes a signed command; the tasma's signed answer settles it
// ("Tasmaya işlendi" only with the tasma's own signature). Results are kept
// in the operation books and reach the account when internet returns.
import { hex } from "../core/bytes.ts";
import type { FencePayload, OpTarget, SettingsPayload } from "./deviceOps.ts";
import type { LoraCommand, LoraEvent, LoraServer } from "./loraCarrier.ts";
import { areaCommand, entriesFor, parseResult, polysFor, RESOURCE, settingsCommand, type LoraResult, type Token } from "./loraSign.ts";

type Store = Pick<Storage, "getItem" | "setItem">;
const KEY = "orun.loraTokens.v1";

export interface LocalDeps {
  ops(): OpTarget[];
  key(device: string): { epoch: number; key: Uint8Array } | null;
  reported(device: string): Record<string, unknown> | null;
  opResult(op: string, device: string, state: "gateway" | "applied" | "same" | "rejected", note?: string): Promise<void>;
  settingsApplied(device: string, values: { interval_s: number; ble_window_s: number; tracking: boolean; relay: boolean }): Promise<void>;
  areasApplied(device: string, payload: FencePayload, r: LoraResult): Promise<void>;
  random32(): number;
}

interface Signed { op: string; device: string; resource: number; cid: number; frames: string[]; }

export class LocalLoraSource implements LoraServer {
  private tokens: Record<string, { inc: string; rev: number }> = {};
  private signed = new Map<string, Signed>();   // key: device/resource
  private deps: LocalDeps;
  private store: Store | null;

  constructor(deps: LocalDeps, store: Store | null) {
    this.deps = deps;
    this.store = store;
    try { this.tokens = JSON.parse(store?.getItem(KEY) ?? "{}") ?? {}; } catch { this.tokens = {}; }
  }

  private token(device: string, resource: number): Token | null {
    const t = this.tokens[`${device}/${resource}`];
    return t ? { incarnation: BigInt("0x" + t.inc), revision: t.rev } : null;
  }

  private remember(r: LoraResult): void {
    if (!r.token.incarnation && !r.token.revision) return;
    this.tokens = { ...this.tokens, [`${r.device}/${r.resource}`]: { inc: r.token.incarnation.toString(16), rev: r.token.revision } };
    try { this.store?.setItem(KEY, JSON.stringify(this.tokens)); } catch { /* memory only */ }
  }

  private sign(t: OpTarget): Signed | null {
    const k = this.deps.key(t.device);
    if (!k) return null;
    const resource = t.kind === "settings" ? RESOURCE.settings : RESOURCE.areas;
    const token = this.token(t.device, resource) ?? { incarnation: 0n, revision: 0 };  // unknown: a probe
    const cid = this.deps.random32() >>> 0;
    let frames: Uint8Array[];
    if (t.kind === "settings") {
      const entries = entriesFor((t.payload as SettingsPayload).set ?? {}, this.deps.reported(t.device));
      if (!entries.length) return null;
      frames = [settingsCommand(k.key, t.device, k.epoch, cid, token, entries)];
    } else {
      const polys = polysFor((t.payload as FencePayload).zones ?? []);
      if (polys.length > 8 || polys.reduce((n, p) => n + p.length, 0) > 64) return null;
      frames = areaCommand(k.key, t.device, k.epoch, cid, token, polys);
    }
    return { op: t.op, device: t.device, resource, cid, frames: frames.map((f) => hex(f).toUpperCase()) };
  }

  async loraCommands(): Promise<LoraCommand[]> {
    const out: LoraCommand[] = [];
    const open = this.deps.ops().filter((t) => t.state === "waiting" || t.state === "gateway");
    for (const t of open) {
      const resource = t.kind === "settings" ? RESOURCE.settings : RESOURCE.areas;
      const slot = `${t.device}/${resource}`;
      let s = this.signed.get(slot);
      if (!s || s.op !== t.op) {
        const fresh = this.sign(t);
        if (!fresh) continue;
        s = fresh;
        this.signed.set(slot, s);
      }
      out.push({ device: s.device, resource: s.resource, command_id: s.cid, frames: s.frames });
    }
    return out;
  }

  async loraEvents(events: LoraEvent[]): Promise<void> {
    for (const e of events) {
      if (e.kind === "result") { await this.result(e.hex); continue; }
      if (e.kind !== "queued") continue;
      for (const s of this.signed.values())
        if (s.device === e.device && s.cid === e.command_id) await this.deps.opResult(s.op, s.device, "gateway");
    }
  }

  private async result(text: string): Promise<void> {
    const data = Uint8Array.from(text.match(/../g) ?? [], (h) => parseInt(h, 16));
    if (data.length !== 56) return;
    const device = hex(data.subarray(4, 12)).toUpperCase();
    const k = this.deps.key(device);
    const r = k ? parseResult(k.key, data) : null;
    if (!r) return;   // not the tasma's word
    this.remember(r);
    const slot = `${device}/${r.resource}`;
    const s = this.signed.get(slot);
    if (!s || s.cid !== r.commandId) return;
    this.signed.delete(slot);   // anything else: signed again (against the state now known)
    if (r.outcome === "APPLIED" || r.outcome === "UNCHANGED") {
      await this.deps.opResult(s.op, device, r.outcome === "APPLIED" ? "applied" : "same");
      if (r.resource === RESOURCE.settings) {
        const services = r.serviceMode === 1 ? r.services : 1;
        await this.deps.settingsApplied(device, { interval_s: r.interval_s, ble_window_s: r.ble_window_s,
                                                  tracking: !!(services & 1), relay: !!(services & 2) });
      } else {
        const t = this.deps.ops().find((x) => x.op === s.op && x.device === device);
        if (t) await this.deps.areasApplied(device, t.payload as FencePayload, r);
      }
    } else if (r.outcome === "INVALID" || r.outcome === "ACCESS_DENIED") {
      await this.deps.opResult(s.op, device, "rejected",
        r.outcome === "INVALID" ? "Tasma reddetti: tasma bu değeri kabul etmedi." : "Tasma reddetti: izin yok.");
    }
  }
}

/** Online the account signs (and sees every answer); offline this phone does. */
export function hybridSource(server: LoraServer | null, local: LoraServer): LoraServer {
  return {
    async loraCommands() {
      if (server) { try { return await server.loraCommands(); } catch { /* offline */ } }
      return local.loraCommands();
    },
    async loraEvents(events) {
      if (server) { try { await server.loraEvents(events); return; } catch { /* offline */ } }
      await local.loraEvents(events);
    },
  };
}
