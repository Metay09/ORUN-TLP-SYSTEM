// Açılışta ve arka plandan dönüşte eski veri "güncel" gibi gösterilmesin
// (sahip, 2026-11-14: "Yükleniyor"dan sonra "7 dk önce", bir saniye sonra
// "2 dk önce"; daha önce aynı yüzden turuncu yanıp sönüyordu).
//
// Uygulama açılınca ya da bir dakikadan uzun arka planda kaldıktan sonra
// öne gelince adımlar şunlardır:
//   1. telefondaki defter okunur ve ekrana gelir (birkaç dakika eski olabilir:
//      servis sunucuya ~2 dk'da bir sorar, uygulama ekranda değilken eşitlemez)
//   2. (yalnız açılışta) hesap geri yüklenir; servisin sakladığı son konumlar
//      deftere alınır
//   3. eşitleme: okuma anahtarları, sonra her tasmanın EN YENİ kaydı sunucudan
//      (latestAt), sonra geçmiş sayfaları
//   4. en yeni kayıtlar yazılınca defter yeniden okunur ve ekrana gelir
// 1 ile 4 arasında zaman yazıları ("7 dk önce") yanlıştır: sunucuda daha
// yenisi vardır. Bu aralığa "yenileniyor" denir. Biter:
//   - en yeni kayıtlar sunucudan geldi VE ekran onları gösteriyor, ya da
//   - eşitleme hata verdi (internet yok: eldeki gerçekten en yenisidir), ya da
//   - 10 saniye geçti (yavaş bağlantı: eldekini göster), ya da
//   - telefonda hesap yok (sunucu yok; yalnız gateway kayıtları).

export const REFRESH_LIMIT_MS = 10_000;
/** Bu kadar arka planda kalan uygulama öne gelince yeniden "yenileniyor". */
export const STALE_AFTER_HIDDEN_MS = 60_000;

export interface OpeningInput {
  /** Hesabın geri yüklenip yüklenmediği belli oldu (hesap yoksa da true). */
  accountSettled: boolean;
  signedIn: boolean;
  /** Bu yenilemenin başladığı an (açılış ya da öne geliş). */
  from: number;
  now: number;
  /** Son eşitlemenin en yeni kayıtları yazdığı an; son tam eşitleme; son hata. */
  latestAt: number | null;
  lastAt: number | null;
  errorAt: number | null;
  /** Ekrandaki kayıtlar defterin şu anki hali mi (usePoints settled). */
  screenCurrent: boolean;
  /** Bu yenileme daha önce bitti mi (geçmiş sayfaları gelirken geri açılmasın). */
  doneFor: number;
}

export function refreshDone(i: OpeningInput): boolean {
  const after = (t: number | null) => t !== null && t >= i.from;
  const fresh = after(i.latestAt) || after(i.lastAt);
  return fresh && i.screenCurrent;
}

export function refreshing(i: OpeningInput): boolean {
  if (i.now - i.from >= REFRESH_LIMIT_MS) return false;
  if (!i.accountSettled) return true;
  if (!i.signedIn) return false;
  if (i.doneFor === i.from) return false;
  if (i.errorAt !== null && i.errorAt >= i.from) return false;
  return !refreshDone(i);
}
