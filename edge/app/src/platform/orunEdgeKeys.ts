// Reuse the AndroidKeyStore AES-GCM vault; browser keys stay in RAM only.
import { Capacitor, registerPlugin } from "@capacitor/core";
import { validEdgeKeys, type EdgeKey, type EdgeKeyVault } from "../app/edgeKeys.ts";
interface NativeKeys {
  putEdgeKeys(value: { keys: EdgeKey[] }): Promise<unknown>;
  getEdgeKeys(): Promise<{ keys: unknown }>;
  clearEdgeKeys(): Promise<unknown>;
}
const native = registerPlugin<NativeKeys>("OrunMobileSession");
const android = () => Capacitor.isNativePlatform() && Capacitor.getPlatform() === "android";
export const edgeKeyVault: EdgeKeyVault = {
  async load() { return android() ? validEdgeKeys((await native.getEdgeKeys()).keys) : []; },
  async save(keys) { if (android()) await native.putEdgeKeys({ keys }); },
  async clear() { if (android()) await native.clearEdgeKeys(); },
};
