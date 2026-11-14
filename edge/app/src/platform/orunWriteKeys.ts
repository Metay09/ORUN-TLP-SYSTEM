// KQ-02B: per-account native AndroidKeyStore AES-GCM signing-key vault.
// Browsers keep keys only in process RAM, never localStorage.
import { Capacitor, registerPlugin } from "@capacitor/core";
import { validWriteKeys, type WriteKey, type WriteKeyVault } from "../app/writeKeys.ts";

interface NativeWriteKeys {
  putWriteKeys(data: { keys: WriteKey[] }): Promise<void>;
  getWriteKeys(): Promise<{ keys: unknown }>;
  clearWriteKeys(): Promise<void>;
}
const native = registerPlugin<NativeWriteKeys>("OrunMobileSession");
const android = () => Capacitor.isNativePlatform() && Capacitor.getPlatform() === "android";

export const writeKeyVault: WriteKeyVault = {
  async load() {
    if (!android()) return [];
    const result = await native.getWriteKeys();
    if (!Array.isArray(result?.keys)) throw new Error("Korunan tasma anahtarları okunamadı.");
    return validWriteKeys(result.keys);
  },
  async save(keys) { if (android()) await native.putWriteKeys({ keys }); },
  async clear() { if (android()) await native.clearWriteKeys(); },
};

/** A legacy unscoped plaintext cache is never authorized for migration.
 * Delete it instead of reviving another account's potentially stale keys.
 * Offline writes may require one online refresh after the APK upgrade. */
export function eraseLegacyWriteKeys(): void {
  try {
    if (typeof localStorage !== "undefined")
      localStorage.removeItem("orun.writeKeys.v1");
  } catch { /* unavailable browser storage has no authority */ }
}
