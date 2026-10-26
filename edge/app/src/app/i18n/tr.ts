// Every text the app shows, in one place (scope doc §13: one dictionary, so
// another language can be added later as a second object of the same shape).
// Plain field Turkish; engineering words only under "Teknik ayrıntılar".

const MIN = 60_000;
const HOUR = 60 * MIN;
const DAY = 24 * HOUR;
const MONTHS = ["Oca", "Şub", "Mar", "Nis", "May", "Haz", "Tem", "Ağu", "Eyl", "Eki", "Kas", "Ara"];
const MONTHS_LONG = ["Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran", "Temmuz", "Ağustos", "Eylül", "Ekim", "Kasım", "Aralık"];
const DAYS = ["Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"];
const DIRS = ["kuzey", "kuzeydoğu", "doğu", "güneydoğu", "güney", "güneybatı", "batı", "kuzeybatı"];

const pad = (n: number) => String(n).padStart(2, "0");
const sameDay = (a: Date, b: Date) =>
  a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
export const cap = (s: string) => (s ? s.charAt(0).toLocaleUpperCase("tr-TR") + s.slice(1) : s);
const num = (n: number) => n.toLocaleString("tr-TR");
const dec = (n: number, d = 1) => n.toFixed(d).replace(".", ",");

export const fmt = {
  num,
  dec,
  clock(ms: number): string {
    const d = new Date(ms);
    return `${pad(d.getHours())}:${pad(d.getMinutes())}`;
  },
  /** "az önce", "8 dk önce", "3 sa önce", "dün 14:20", "12 Eki 09:05". */
  ago(ms: number, now: number): string {
    const d = Math.max(0, now - ms);
    if (d < MIN) return "az önce";
    if (d < HOUR) return `${Math.floor(d / MIN)} dk önce`;
    const then = new Date(ms), today = new Date(now);
    if (d < 12 * HOUR || sameDay(then, today)) return `${Math.floor(d / HOUR)} sa önce`;
    if (sameDay(then, new Date(now - DAY))) return `dün ${fmt.clock(ms)}`;
    return `${then.getDate()} ${MONTHS[then.getMonth()]} ${fmt.clock(ms)}`;
  },
  /** "40 dakikadır", "2 saattir", "3 gündür". */
  since(ms: number, now: number): string {
    const d = Math.max(0, now - ms);
    if (d < HOUR) return `${Math.max(1, Math.floor(d / MIN))} dakikadır`;
    if (d < 2 * DAY) return `${Math.floor(d / HOUR)} saattir`;
    return `${Math.floor(d / DAY)} gündür`;
  },
  /** "2 sa 40 dk", "35 dk", "3 gün 4 sa". */
  duration(ms: number): string {
    const m = Math.round(Math.max(0, ms) / MIN);
    if (m < 60) return `${m} dk`;
    if (m < 48 * 60) return m % 60 ? `${Math.floor(m / 60)} sa ${m % 60} dk` : `${Math.floor(m / 60)} sa`;
    const h = Math.round(m / 60);
    return h % 24 ? `${Math.floor(h / 24)} gün ${h % 24} sa` : `${Math.floor(h / 24)} gün`;
  },
  /** "Bugün", "Dün", "12 Ekim Pazartesi". */
  day(ms: number, now: number): string {
    const d = new Date(ms);
    if (sameDay(d, new Date(now))) return "Bugün";
    if (sameDay(d, new Date(now - DAY))) return "Dün";
    return `${d.getDate()} ${MONTHS_LONG[d.getMonth()]} ${DAYS[d.getDay()]}`;
  },
  dateShort(ms: number): string {
    const d = new Date(ms);
    return `${d.getDate()} ${MONTHS[d.getMonth()]}`;
  },
  dateTime(ms: number): string {
    const d = new Date(ms);
    return `${d.getDate()} ${MONTHS[d.getMonth()]} ${fmt.clock(ms)}`;
  },
  distance(m: number): string {
    if (m < 1000) return `${Math.max(1, Math.round(m / 10) * 10)} m`;
    return `${dec(m / 1000, m < 10000 ? 1 : 0)} km`;
  },
  /** Big-number form for the find screen: value and unit apart. */
  distanceParts(m: number): [string, string] {
    if (m < 1000) return [String(Math.max(1, Math.round(m / 5) * 5)), "m"];
    return [dec(m / 1000, m < 10000 ? 2 : 1), "km"];
  },
  direction(deg: number): string {
    return DIRS[Math.round(deg / 45) % 8];
  },
  period(s: number): string {
    if (s < 90) return `${Math.round(s)} sn`;
    if (s < 5400) return `${Math.round(s / 60)} dk`;
    return `${dec(s / 3600, s % 3600 ? 1 : 0)} sa`;
  },
  coords(lat: number, lon: number): string {
    return `${lat.toFixed(5)}, ${lon.toFixed(5)}`;
  },
};

export const tr = {
  app: { name: "ORUN", demoBadge: "Tanıtım sürüsü", demoBanner: "Örnek sürü gösteriliyor · gerçek kayıt değil" },
  tabs: { map: "Harita", herd: "Sürü", zones: "Alanlar", events: "Olaylar", network: "Ağ" },
  common: {
    back: "Geri", close: "Kapat", save: "Kaydet", cancel: "Vazgeç", edit: "Düzenle", more: "Tümü",
    on: "Açık", off: "Kapalı", unknown: "bilinmiyor", none: "—", settings: "Ayarlar", ok: "Tamam",
    tech: "Teknik ayrıntılar", showOnMap: "Haritada göster", directions: "Yol tarifi",
  },
  states: {
    opening: "Açılıyor…",
    ledgerError: (e: string) => `Telefondaki kayıt defteri açılamadı: ${e}`,
    noAnimals: "Henüz hayvan yok",
    noAnimalsBody: "İstasyona bağlanınca tasmaların gönderdiği konumlar burada görünür. Kayıtlar telefonda saklanır, internet gerekmez.",
    connect: "İstasyona bağlan",
    tryDemo: "Tanıtım sürüsünü aç",
    offlineMap: "Harita görüntüsü yüklenemedi · hayvanlar yine de çizilir",
  },
  // ---------------------------------------------------------------- herd ---
  status: {
    waiting: "İlk konum bekleniyor",
    noClock: "Konum var · saat bilgisi yok",
    seenAgo: (ago: string) => `${cap(ago)} görüldü`,
    silentFor: (since: string) => `${cap(since)} haber yok`,
    lostFor: (since: string) => `${cap(since)} haber yok · kayıp olabilir`,
    short: { waiting: "Bekleniyor", lost: "Kayıp olabilir", silent: (d: string) => `${d} sessiz` },
    batteryCritical: "Pil bitiyor",
    batteryLow: "Pil azalıyor",
  },
  herd: {
    title: "Sürü",
    search: "Ad, küpe no ya da tasma",
    add: "Yeni tasma ekle",
    filters: { all: "Hepsi", attention: "Bakılacak", silent: "Sessiz", battery: "Pil" },
    summaryEmpty: "Henüz hayvan yok",
    summaryWaiting: (n: number) => `${n} tasma · ilk konum bekleniyor`,
    summaryLook: (n: number, look: number) => `${n} hayvan · ${look} tanesine bakın`,
    summaryAllFresh: (n: number) => `${n} hayvan · hepsi yakın zamanda görüldü`,
    summaryNoneFresh: (n: number) => `${n} hayvan · yeni konum gelmedi`,
    summarySome: (n: number, fresh: number) => `${n} hayvan · ${fresh} tanesi yakın zamanda görüldü`,
    noMatch: "Aramaya uyan hayvan yok",
    ringHelp: "Halka son konumun tazeliğini gösterir: dolu halka az önce ölçüldü demek, boşaldıkça konum eskir.",
    today: "Bugün, sürü",
    avgActive: "ortalama hareket",
    avgWalk: "ortalama yol",
    located: (n: number) => `${n} hayvanın konumu var`,
  },
  animal: {
    tabs: { today: "Bugün", history: "Geçmiş", info: "Bilgiler" },
    find: "Bul",
    map: "Harita",
    addNote: "Not ekle",
    lastPosition: "Son konum",
    measured: "Ölçüm",
    arrived: "Telefona geldi",
    delay: (d: string) => `${d} gecikmeyle geldi`,
    quality: (sats: number | null, hdop: number | null) =>
      sats === null ? "ölçüm kalitesi bilinmiyor"
        : `${sats} uydu${hdop !== null ? ` · hassasiyet ${hdop <= 1.5 ? "iyi" : hdop <= 3 ? "orta" : "zayıf"}` : ""}`,
    activity: "Hareket",
    activityToday: (d: string) => `Bugün ${d} hareket etti`,
    activityNone: "Bu tasma hareket özeti göndermiyor",
    activityStripHelp: "Her çizgi bir saat: koyu çizgi, o saatte daha çok hareket demek.",
    walked: "Yürüdüğü yol",
    walkedToday: (d: string) => `Bugün yaklaşık ${d}`,
    walkedHelp: "Kayıtlar arası düz çizgiyle hesaplanır; gerçek yol biraz daha uzundur.",
    battery: "Pil",
    batteryUnknown: "Bu tasma pil bilgisi göndermiyor",
    batteryGood: (p: number | null) => `Pil iyi${p !== null ? ` · %${p}` : ""}`,
    batteryLow: (p: number | null) => `Pil azalıyor${p !== null ? ` · %${p}` : ""}`,
    batteryCritical: "Pil bitmek üzere · bugün şarj edin",
    batteryTrend: "Son 7 gün",
    days: "Günler",
    dayLine: (records: number, walked: string, active: string | null) =>
      `${records} kayıt · ${walked}${active ? ` · ${active} hareket` : ""}`,
    dayOnMap: "Bu günü haritada oynat",
    noHistory: "Henüz geçmiş yok",
    events: "Olaylar",
    noEvents: "Bu hayvan için olay yok",
    identity: "Kimlik",
    fields: { name: "Adı", tag: "Küpe no", breed: "Irkı", sex: "Cinsiyeti", birth: "Doğum tarihi", color: "Rengi", note: "Not" },
    sex: { female: "Dişi", male: "Erkek", "": "—" },
    age: (months: number) => (months < 24 ? `${months} aylık` : `${Math.floor(months / 12)} yaşında`),
    notebook: "Defter",
    notebookEmpty: "Sağlık, doğum, aşı ve tedavi notlarınız burada tarihleriyle durur.",
    notePlaceholder: "Örn. Şap aşısı yapıldı",
    device: "Tasma",
    deviceLine: (short: string) => `Tasma ${short}`,
    reportEvery: (p: string) => `rapor aralığı ${p}`,
    records: (n: number) => `${num(n)} kayıt telefonda`,
    noDevice: "Bu hayvana tasma atanmamış",
  },
  edit: {
    title: "Hayvan bilgileri",
    namePlaceholder: (short: string) => `Örn. Sarıkız (tasma ${short})`,
    tagPlaceholder: "TR 09 1234 5678",
    breedPlaceholder: "Örn. Simental",
    swatch: "Renk seç",
  },
  add: {
    title: "Yeni tasma ekle",
    body: "Tasma ilk kez bilgisayardaki ORUN kurulum aracıyla hazırlanır; anahtarı orada kapalı olarak yazılır.",
    after: "Kurulan tasma istasyona ilk konumunu gönderdiğinde Sürü'de kendiliğinden görünür. Adını, küpe numarasını ve rengini buradan verirsiniz.",
  },
  // ----------------------------------------------------------------- map ---
  map: {
    search: "Hayvan ara",
    layers: "Harita katmanları",
    myLocation: "Konumum",
    fitHerd: "Bütün sürüyü göster",
    tilt: "3B görünüm",
    north: "Kuzeyi yukarı al",
    base: { title: "Harita türü", satellite: "Uydu", terrain: "Arazi", street: "Harita" },
    layer: {
      title: "Katmanlar",
      animals: "Hayvanlar", trail: "İz", heat: "Yoğunluk", coverage: "Kapsama",
      stations: "İstasyon", me: "Konumum", zones: "Alanlar", labels: "Yer adları", relief: "Rölyef gölgesi",
    },
    layerHelp: {
      heat: "Hayvanların en çok bulunduğu yerler",
      coverage: "5 m karelerde istasyonun duyduğu sinyal",
      trail: "Seçili hayvanın seçilen dönemdeki izi; boşluklar kesik çizgiyle",
      zones: "Alan yok",
    },
    range: { title: "Dönem", today: "Bugün", h24: "24 saat", d3: "3 gün", d7: "7 gün", d30: "30 gün" },
    tilesNote: "Harita görüntüsü internetle gelir; hayvanların yeri internetsiz de görünür.",
    zoomForCoverage: "Kapsama karelerini görmek için yakınlaştırın",
    coverageLegend: { strong: "Güçlü", medium: "Orta", weak: "Zayıf", none: "Bağlantı yoktu" },
    heatLegend: "Az ← → Çok",
    noTrail: "Bu dönemde iz yok",
    play: "Oynat", pause: "Durdur",
    recordOf: (i: number, n: number) => `kayıt ${i}/${n}`,
    distance: "Uzaklık",
    now: "Şu an",
    doing: { moving: "Hareketli", little: "Az hareketli", resting: "Dinleniyor" },
    profile: "Profil",
    attentionLine: (names: string[]) => names.join(" · "),
    phone: "Telefonunuz",
    stationHere: "İstasyon",
    cluster: (n: number) => `${n} hayvan`,
    attribution: "Harita kaynakları",
  },
  find: {
    title: (name: string) => `${name} · bul`,
    up: (m: number) => `${Math.round(m)} m yukarıda`,
    down: (m: number) => `${Math.round(m)} m aşağıda`,
    level: "aynı yükseklikte",
    lastFix: (ago: string) => `Son konum ${ago}`,
    moved: "Hayvan o zamandan beri yer değiştirmiş olabilir",
    quality: (sats: number | null) => (sats ? `ölçüm ${sats} uydu` : "ölçüm kalitesi bilinmiyor"),
    noPhone: "Telefonun konumu bekleniyor…",
    noPhoneHelp: "Konum izni verin ve açık bir yerde durun.",
    accuracy: (m: number) => `Telefonun konumu ±${Math.round(m)} m`,
    noCompass: "Bu telefonda pusula yok: ok kuzeye göre çizildi",
    compassHelp: "Telefonu yere paralel tutun; ok hayvanın yönünü gösterir.",
    near: "Hemen yanınızda",
    noPosition: "Bu hayvanın konumu yok",
  },
  // --------------------------------------------------------------- zones ---
  zones: {
    title: "Alanlar",
    empty: "Henüz alan yok",
    emptyBody: "Alan, hayvanların kalması gereken mera sınırıdır. Tasma sınırı kendisi denetler; dışarı çıkınca daha sık konum gönderir ve uyarı verir.",
    rules: "Bir tasmada en fazla 8 alan ve toplam 64 köşe olabilir.",
  },
  // -------------------------------------------------------------- events ---
  events: {
    title: "Olaylar",
    filters: { all: "Hepsi", alerts: "Uyarılar", animals: "Hayvanlar", system: "Cihaz ve ağ" },
    now: "Şu an",
    nothingNow: "Şu an bakılacak bir şey yok",
    empty: "Henüz olay yok",
    emptyBody: "Hayvanlardan, istasyondan ve sunucudan gelen her önemli gelişme burada sırayla görünür.",
    seen: "Gördüm",
    seenDone: "Görüldü",
    measured: (c: string) => `ölçüm ${c}`,
    lastFix: (c: string) => `son konum ${c}`,
    span: (a: string, b: string) => `${a} – ${b}`,
    backAgain: "sonra yeniden görüldü",
    movedAgain: "sonra yeniden hareket etti",
    started: (c: string) => `başlangıç ${c}`,
    arrived: (c: string) => `telefona ${c}`,
    resolved: (c: string) => `çözüldü · ${c}`,
    ongoing: "sürüyor",
    silent: { title: (n: string) => `${n} sessiz`, detail: (d: string, k: number) => `${d} boyunca konum gelmedi · ${k} rapor kaçtı` },
    silentNow: { title: (n: string) => `${n} sessiz`, detail: (since: string, k: number) => `${cap(since)} konum gelmiyor · ${k} rapor kaçtı` },
    lost: { title: (n: string) => `${n} kayıp olabilir`, detail: (since: string) => `${cap(since)} hiç haber yok · son konumuna bakın` },
    weakHint: "Son konumu zayıf kapsama yerindeydi; istasyon orayı iyi duymuyor olabilir.",
    batteryLow: { title: (n: string) => `${n}: pil azalıyor`, detail: (p: string) => `Pil ${p}` },
    batteryCritical: { title: (n: string) => `${n}: pil bitiyor`, detail: "Tasma yakında susacak · bugün şarj edin" },
    still: { title: (n: string) => `${n} hareketsiz`, detail: (d: string) => `${d} boyunca hiç hareket etmedi` },
    stationFull: { title: "İstasyonun belleği doluyor", detail: (h: number, c: number) => `${num(h)}/${num(c)} kayıt telefona aktarılmayı bekliyor` },
    serverDown: { title: "Sunucuya ulaşılamıyor", detail: (n: number) => `${num(n)} kayıt telefonda güvende · internet gelince gönderilecek` },
  },
  // ------------------------------------------------------------- network ---
  network: {
    title: "Ağ",
    chain: "Kayıt güvende mi?",
    nodes: { collars: "Tasmalar", station: "İstasyon", phone: "Telefon", server: "Sunucu" },
    collarsLine: (n: number, fresh: number) => `${n} tasma · ${fresh} taze`,
    stationHeld: (n: number) => (n ? `${num(n)} bekliyor` : "bekleyen yok"),
    stationOff: "bağlı değil",
    phoneLine: (n: number) => `${num(n)} kayıt`,
    serverLine: (pending: number) => (pending ? `${num(pending)} gönderilecek` : "hepsi gönderildi"),
    serverOff: "ayarlanmadı",
    chainHelp: "Bir kayıt ancak sonraki halka onu kalıcı yazdıktan sonra öncekinden silinir.",
    station: "İstasyon",
    devices: "Cihazlar",
    devicesNone: "Henüz cihaz görülmedi. İstasyona bağlanınca, istasyonun duyduğu tasmalar burada listelenir.",
    coverage: "Kapsama",
    coverageLine: (cells: number, strong: number, weak: number) =>
      `${num(cells)} kare ölçüldü · %${strong} güçlü · %${weak} zayıf`,
    coverageNone: "Henüz kapsama ölçümü yok. İstasyon bağlıyken gelen her canlı konum bir kare ölçer.",
    server: "Sunucu",
  },
  station: {
    connected: (via: string) => `İstasyona bağlı · ${via}`,
    connectedShort: "İstasyona bağlı",
    searching: "İstasyon aranıyor…",
    waiting: "İstasyon bekleniyor · yeniden bağlanacak",
    notConnected: "İstasyona bağlı değil",
    ble: "Bluetooth", usb: "USB",
    connectBle: "Bluetooth ile bağlan",
    connectUsb: "USB kablosuyla bağlan",
    pickOther: "Başka istasyon seç",
    disconnect: "Bağlantıyı kes",
    held: "İstasyonda bekleyen",
    memory: "İstasyon belleği",
    background: "Bağlıyken ekran kapansa da kayıtlar gelmeye devam eder. İstasyon bir kaydı ancak telefon onu kalıcı yazdıktan sonra siler.",
    position: "Yeri",
    positionNone: "İşaretlenmedi",
    markHere: "Şu an istasyonun yanındayım",
    markHelp: "Telefonun konumu istasyonun yeri olarak kaydedilir; harita ve kapsama için kullanılır.",
    clearPosition: "Yerini sil",
    id: "İstasyon kimliği",
  },
  server: {
    title: "Sunucu",
    notSet: "Sunucu ayarlanmadı · kayıtlar telefonda saklı",
    sending: "Gönderiliyor…",
    pending: (n: number, offline: boolean) => `${num(n)} kayıt telefonda · ${offline ? "internet gelince" : "birazdan"} gönderilecek`,
    allSent: (at: string | null) => (at ? `Tüm kayıtlar gönderildi · ${at}` : "Tüm kayıtlar gönderildi"),
    address: "Adres", key: "Anahtar", auto: "Kendiliğinden gönder",
    sendNow: "Şimdi gönder",
    lastError: (e: string) => `Son deneme: ${e}`,
  },
  device: {
    title: (short: string) => `Tasma ${short}`,
    services: "Hizmetler",
    service: { location: "Konum takibi", activity: "Hareket özeti", battery: "Pil bildirimi" },
    serviceState: { running: "çalışıyor", silent: "sessiz", off: "göndermiyor" },
    install: { animal: "Hayvana takılı", fixed: "Sabit", vehicle: "Araçta" },
    signal: "Sinyal",
    signalLine: (rssi: number, snr: number | null) => `${rssi} dBm${snr !== null ? ` · SNR ${dec(snr, 1)}` : ""}`,
    signalNone: "Son kayıt canlı alınmadı",
    link: { live: "Doğrudan, canlı", through: "İstasyon üzerinden, gecikmeli", cut: "Bağlantı yokken kaydedildi" },
    lastContact: "Son haberleşme",
    report: "Rapor aralığı",
    records: "Telefondaki kayıt",
    delay: "Ortalama gecikme",
    attached: "Takılı olduğu",
  },
  // ------------------------------------------------------------ settings ---
  settings: {
    title: "Ayarlar",
    look: "Görünüm",
    theme: "Tema",
    themes: { auto: "Otomatik", light: "Açık", dark: "Koyu" },
    sun: "Güneş modu",
    sunHelp: "Güneş altında okumak için daha koyu yazı ve kalın işaretler.",
    data: "Veri",
    demo: "Tanıtım sürüsü",
    demoHelp: "Cihaz bağlı değilken uygulamayı örnek bir sürüyle gösterir. Gerçek kayıtlarınız etkilenmez; açıkken üstte 'Tanıtım sürüsü' yazar.",
    phoneRecords: (n: number) => `Telefonda ${num(n)} kayıt · hiçbiri silinmez`,
    about: "Hakkında",
    version: "Sürüm",
    sources: "Harita: Esri World Imagery, OpenTopoMap, OpenFreeMap, © OpenStreetMap katkıcıları. Yükselti: AWS Terrain Tiles (Mapzen).",
  },
  tech: {
    gateway: "İstasyon kimliği", held: "İstasyonun tuttuğu", session: "Bu oturumda yeni / vardı",
    handoff: "Teslim / bekleyen teslim", rejected: "Red / çakışma / belirsiz", phone: "Telefondaki kayıt",
    server: "Sunucuda / bekleyen", storage: "Depolama", log: "Kayıt günlüğü",
  },
};

export type Dict = typeof tr;
