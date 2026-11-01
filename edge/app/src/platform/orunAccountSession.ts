// 1b: hesap oturumunu uygulama kapanıp açılınca da tutar.
// Android'de AndroidKeyStore AES-GCM ile şifreli, yedeklenmeyen klasörde
// (OrunMobileSession eklentisinin ayrı dosyası). Tarayıcı depolaması
// (localStorage/IndexedDB) KULLANILMAZ; Android dışında saklama yapılmaz.
import { Capacitor, registerPlugin } from "@capacitor/core";
import type { AccountIdentity } from "../app/ownerAccountClient.ts";

export interface StoredAccount {
  origin: string;
  identity: AccountIdentity;
  accessToken: string;
}

interface NativeAccount {
  putAccount(value: {
    origin: string; site: string; actor: string; installation: string;
    username: string; accessToken: string; expiresAt: number;
  }): Promise<{ stored: boolean }>;
  getAccount(): Promise<{
    present: boolean; origin?: string; site?: string; actor?: string;
    installation?: string; username?: string; accessToken?: string; expiresAt?: number;
  }>;
  clearAccount(): Promise<{ cleared: boolean }>;
}

const plugin = registerPlugin<NativeAccount>("OrunMobileSession");
const TOKEN = /^[A-Za-z0-9_-]{48,128}$/;

function android(): boolean {
  return Capacitor.isNativePlatform() && Capacitor.getPlatform() === "android";
}

export async function storeAccount(origin: string, identity: AccountIdentity,
                                   accessToken: string): Promise<void> {
  if (!android()) return;
  await plugin.putAccount({
    origin, site: identity.site, actor: identity.actor,
    installation: identity.installation, username: identity.username,
    accessToken, expiresAt: identity.expiresAt,
  });
}

export async function loadAccount(origin: string): Promise<StoredAccount | null> {
  if (!android()) return null;
  const a = await plugin.getAccount().catch(() => null);
  if (!a?.present || a.origin !== origin || typeof a.accessToken !== "string" ||
      !TOKEN.test(a.accessToken) || typeof a.site !== "string" ||
      typeof a.actor !== "string" || typeof a.installation !== "string" ||
      typeof a.username !== "string" || typeof a.expiresAt !== "number" ||
      a.expiresAt <= Date.now() / 1000)
    return null;
  return {
    origin,
    accessToken: a.accessToken,
    identity: {
      site: a.site, actor: a.actor, installation: a.installation,
      username: a.username, expiresAt: a.expiresAt,
    },
  };
}

export async function forgetAccount(): Promise<void> {
  if (!android()) return;
  await plugin.clearAccount().catch(() => undefined);
}
