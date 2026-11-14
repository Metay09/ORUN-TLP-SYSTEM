// v1 step 7: the native Gateway bridge (android GatewayBridgeService /
// BridgeRuntime / OrunGatewayBridgePlugin). Android only; null elsewhere.
import { Capacitor, registerPlugin, type PluginListenerHandle } from "@capacitor/core";
import type { BridgeRow } from "../core/bridgeImport.ts";
import type { ReadKey } from "../core/keys.ts";

export type BridgeState = "off" | "adapter_off" | "connecting" | "waiting" | "ready";

export interface BridgeStatus {
  enabled: boolean;
  address: string | null;
  state: BridgeState;
  detail: string | null;
  permitted?: boolean;
  gateway?: string | null;
  held?: number | null;
  capacity?: number | null;
  counts?: Record<"STORED" | "DUPLICATE" | "REJECTED" | "CONFLICT" | "UNCERTAIN", number>;
  handedOff?: number;
  acceptRejected?: number;
  pendingAccepts?: number;
  keyMissing?: boolean;
  records?: number;
  uploaded?: number;
  pending?: number;
  conflict?: number;
  rejected?: number;
  lastUploadAgoMs?: number | null;
  uploadError?: string | null;
  connectedAt?: number | null;
  db?: string;
  storage?: string;
  loraSeq?: number;
  log?: string[];
}

export interface BridgePage { db: string; rows: BridgeRow[]; last: number; }

export interface GatewayBridgePlugin {
  start(o: { address: string }): Promise<void>;
  stop(): Promise<void>;
  forget(): Promise<void>;
  resume(): Promise<void>;
  status(): Promise<BridgeStatus>;
  takeRecords(o: { db: string; after: number; limit?: number }): Promise<BridgePage>;
  taken(o: { db: string; rev: number }): Promise<void>;
  takeLora(o: { after: number }): Promise<{ lines: string[]; last: number }>;
  sendLine(o: { line: string }): Promise<void>;
  redump(): Promise<void>;
  uploadNow(): Promise<void>;
  setReadKeys(o: { keys: ReadKey[] }): Promise<void>;
  addListener(event: "change", fn: () => void): Promise<PluginListenerHandle>;
}

export const nativeGatewayBridge: GatewayBridgePlugin | null =
  Capacitor.isNativePlatform() && Capacitor.getPlatform() === "android"
    ? registerPlugin<GatewayBridgePlugin>("OrunGatewayBridge") : null;
