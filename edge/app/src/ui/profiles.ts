// Animal names/colours/notes the owner gives his collars. Kept on this phone
// (localStorage; survives app restarts). Moving them to the server so every
// phone shares them is later work (owner accounts, step 3C+).
import type { Profile, Profiles } from "./herd.ts";

const KEY = "orun.edge.animals";

function load(): Profiles {
  try {
    const v = JSON.parse(localStorage.getItem(KEY) ?? "{}");
    return v && typeof v === "object" ? v : {};
  } catch {
    return {};
  }
}

export class ProfileStore {
  private data: Profiles = load();
  private listeners = new Set<() => void>();

  get all(): Profiles { return this.data; }

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => { this.listeners.delete(fn); };
  }

  set(id: string, patch: Profile): void {
    const next = { ...(this.data[id] ?? {}), ...patch };
    if (next.name !== undefined) next.name = next.name.trim().slice(0, 40);
    if (next.note !== undefined) next.note = next.note.slice(0, 500);
    this.data = { ...this.data, [id]: next };
    try { localStorage.setItem(KEY, JSON.stringify(this.data)); } catch { /* memory only */ }
    for (const fn of this.listeners) fn();
  }
}
