package tr.orun.edge;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.List;

/**
 * One Gateway over Bluetooth, kept connected for as long as the owner wants
 * this phone to be its bridge (v1 step 7). Pure logic on one thread: the
 * service runs every call on its worker thread and AndroidRadio turns the
 * BluetoothGatt callbacks into the on*() calls. No Android types, so the PC
 * check drives it against a simulated Gateway (BridgeLinkCheck).
 *
 * Connection policy (what Android BLE apps that must stay connected do):
 * a direct attempt (20 s); after two failed direct attempts a background
 * auto-connect that completes whenever the Gateway is in range again, with
 * a direct attempt now and then (every 2 min, growing to 10 min) in case the
 * phone's background connect stalls. Bluetooth off: wait for it to come back.
 * Every drop ends the custody session (BridgeCore.sessionEnd): nothing in
 * flight survives a reconnect, the Gateway sends its records again.
 */
final class BridgeLink {
    interface Radio {
        boolean adapterOn();
        /** Starts a connection attempt; false if it could not even start. */
        boolean connect(boolean auto);
        /** -> onServices(found) */
        void discover();
        /** Clears Android's cached service table first. -> onServices(found) */
        void refreshAndDiscover();
        /** -> onEnabled(ok) */
        boolean enableIndications();
        /** One 20-byte frame, write with response. -> onWritten(ok) */
        boolean write(byte[] frame);
        void priority(boolean high);
        /** Drops the link and its GATT object. No callback follows. */
        void close();
    }

    interface Timers {
        long nowMs();
        void after(long ms, Runnable r);
    }

    interface Sessions {
        void alive(long nowSec) throws Exception;
        void end(long nowSec) throws Exception;
    }

    interface Events {
        void state(String state, String detail);
        /** Counters or held records changed. */
        void changed();
        /** n new records are on the phone now (upload soon). */
        void stored(int n);
        void hello(String gatewayHex);
        void loraLine(String line);
        void log(String line);
    }

    interface Done { void done(boolean ok); }

    static final String OFF = "off", ADAPTER_OFF = "adapter_off", CONNECTING = "connecting",
            WAITING = "waiting", READY = "ready";
    static final long CONNECT_TIMEOUT_MS = 20_000, STEP_TIMEOUT_MS = 15_000, WRITE_TIMEOUT_MS = 10_000,
            RETRY_MS = 5_000, NO_SERVICE_RETRY_MS = 60_000, AUTO_FIRST_MS = 120_000, AUTO_MAX_MS = 600_000,
            TICK_MS = 250, ALIVE_MS = 15_000, QUIET_MS = 20_000, WRITE_BUSY_MS = 30,
            DUMP_EVERY_MS = 3_600_000, INCOMPLETE_REDUMP_MS = 10_000;
    static final int DIRECT_TRIES = 2, WRITE_BUSY_TRIES = 100, INCOMPLETE_REDUMPS = 3;
    static final byte LORA_LINE_TO_GATEWAY = (byte) 0x83;

    private enum Step { OFF, ADAPTER_OFF, CONNECTING, AUTO, BACKOFF, DISCOVERING, ENABLING, READY }

    private static final class Out {
        final byte[] message;
        final Done done;
        Out(byte[] message, Done done) { this.message = message; this.done = done; }
    }

    private final Radio radio;
    private final Timers timers;
    private final BridgeCore core;
    private final Sessions sessions;
    private final Events events;

    private Step step = Step.OFF;
    private String detail;
    private int epoch;               // every attempt / drop: older timers do nothing
    private int directFails;
    private long autoDelay = AUTO_FIRST_MS;
    private boolean refreshed;
    private final EdgeWire.Reassembler rx = new EdgeWire.Reassembler();
    private final Deque<Out> outbox = new ArrayDeque<>();
    private Out writing;
    private List<byte[]> writingFrames;
    private int writingIndex, busyTries, writeToken;
    private int messageId;
    private long lastAliveMs, lastBusyMs, lastDumpMs;
    private int incompleteRuns;
    private boolean high;
    long connectedAtMs = -1;

    BridgeLink(Radio radio, Timers timers, BridgeCore core, Sessions sessions, Events events) {
        this.radio = radio; this.timers = timers; this.core = core; this.sessions = sessions; this.events = events;
    }

    /** off | adapter_off | connecting | waiting | ready */
    String state() {
        switch (step) {
            case OFF: return OFF;
            case ADAPTER_OFF: return ADAPTER_OFF;
            case READY: return READY;
            case CONNECTING: return probing ? WAITING : CONNECTING;
            case DISCOVERING: case ENABLING: return CONNECTING;
            default: return WAITING;
        }
    }

    String detail() { return detail; }

    boolean ready() { return step == Step.READY; }

    private void setStep(Step s, String why) {
        String before = state(), beforeDetail = detail;
        step = s;
        detail = why;
        if (!state().equals(before) || (why == null ? beforeDetail != null : !why.equals(beforeDetail)))
            events.state(state(), detail);
    }

    // ---- owner / system ------------------------------------------------------
    void start() {
        if (step != Step.OFF) return;
        directFails = 0;
        probing = false;
        autoDelay = AUTO_FIRST_MS;
        if (!radio.adapterOn()) { setStep(Step.ADAPTER_OFF, "Bluetooth kapalı"); return; }
        attempt(false);
    }

    void stop() {
        if (step == Step.OFF) return;
        endSession();
        epoch++;
        radio.close();
        setStep(Step.OFF, null);
    }

    void onAdapter(boolean on) {
        if (step == Step.OFF) return;
        if (!on) {
            endSession();
            epoch++;
            radio.close();
            setStep(Step.ADAPTER_OFF, "Bluetooth kapalı");
        } else if (step == Step.ADAPTER_OFF) {
            directFails = 0;
            probing = false;
            autoDelay = AUTO_FIRST_MS;
            attempt(false);
        }
    }

    // ---- connecting ----------------------------------------------------------
    private static final String AWAY = "Gateway'e ulaşılamıyor; menzile girince bağlanacak";
    private boolean probing;        // a direct try while waiting in the background: still "waiting"

    private void attempt(boolean auto) {
        final int e = ++epoch;
        radio.close();
        refreshed = false;
        if (!radio.adapterOn()) { setStep(Step.ADAPTER_OFF, "Bluetooth kapalı"); return; }
        if (!radio.connect(auto)) {
            backoff("Bluetooth bağlantısı başlatılamadı; yeniden denenecek");
            return;
        }
        if (auto) {
            // Waits in the background until the Gateway is in range; a direct
            // attempt now and then in case this phone's background connect stalls.
            probing = false;
            setStep(Step.AUTO, AWAY);
            final long wait = autoDelay;
            autoDelay = Math.min(AUTO_MAX_MS, autoDelay * 2);
            timers.after(wait, () -> {
                if (e == epoch && step == Step.AUTO) { probing = true; attempt(false); }
            });
        } else {
            setStep(Step.CONNECTING, directFails >= DIRECT_TRIES ? AWAY : null);
            timers.after(CONNECT_TIMEOUT_MS, () -> { if (e == epoch && step == Step.CONNECTING) failed("bağlanılamadı"); });
        }
    }

    /** Always a pause before the next try: a radio that fails at once must not spin. */
    private void backoff(String why) {
        final int e = ++epoch;
        radio.close();
        setStep(Step.BACKOFF, directFails >= DIRECT_TRIES ? AWAY : why);
        timers.after(RETRY_MS, () -> {
            if (e == epoch && step == Step.BACKOFF) attempt(directFails >= DIRECT_TRIES);
        });
    }

    private void failed(String why) {
        endSession();
        directFails++;
        probing = false;
        backoff(why + "; yeniden deneniyor");
    }

    void onConnected() {
        if (step != Step.CONNECTING && step != Step.AUTO) return;
        final int e = ++epoch;
        probing = false;
        setStep(Step.DISCOVERING, null);
        high = true;
        radio.priority(true);
        timers.after(STEP_TIMEOUT_MS, () -> { if (e == epoch && step == Step.DISCOVERING) failed("servis listesi alınamadı"); });
        // A short pause before discovery: some phones lose the first request otherwise.
        timers.after(300, () -> { if (e == epoch && step == Step.DISCOVERING) radio.discover(); });
    }

    void onServices(boolean found) {
        if (step != Step.DISCOVERING) return;
        if (!found && !refreshed) {
            // Android keeps the service table of a device it saw before (e.g.
            // with older firmware): clear it and look again once.
            refreshed = true;
            events.log("[BLE] telefondaki servis listesi eski; yenileniyor");
            radio.refreshAndDiscover();
            return;
        }
        if (!found) {
            endSession();
            final int e = ++epoch;
            radio.close();
            setStep(Step.BACKOFF, "gateway'de ORUN Edge servisi yok (eski yazılım?)");
            timers.after(NO_SERVICE_RETRY_MS, () -> { if (e == epoch && step == Step.BACKOFF) attempt(false); });
            return;
        }
        final int e = ++epoch;
        setStep(Step.ENABLING, null);
        if (!radio.enableIndications()) { failed("bildirimler açılamadı"); return; }
        timers.after(STEP_TIMEOUT_MS, () -> { if (e == epoch && step == Step.ENABLING) failed("bildirimler açılamadı"); });
    }

    void onEnabled(boolean ok) {
        if (step != Step.ENABLING) return;
        if (!ok) { failed("bildirimler açılamadı"); return; }
        final int e = ++epoch;
        rx.reset();
        outbox.clear();
        writing = null;
        directFails = 0;
        autoDelay = AUTO_FIRST_MS;
        connectedAtMs = timers.nowMs();
        lastBusyMs = connectedAtMs;
        lastDumpMs = connectedAtMs;          // the Gateway sends everything it holds on connect
        incompleteRuns = 0;
        alive(true);
        setStep(Step.READY, null);
        events.log("[BLE] bağlandı");
        tick(e);
    }

    void onDisconnected(int status) {
        if (step == Step.OFF || step == Step.ADAPTER_OFF || step == Step.BACKOFF) return;
        if (step != Step.READY) { failed("bağlantı kurulamadı (" + status + ")"); return; }
        events.log("[BLE] bağlantı koptu (" + status + ")");
        endSession();
        directFails = 0;
        backoff("bağlantı koptu; yeniden deneniyor");
    }

    private void endSession() {
        if (step == Step.READY) {
            try { sessions.end(timers.nowMs() / 1000); } catch (Exception ignored) { }
        }
        core.sessionEnd();
        rx.reset();
        Out w = writing;
        writing = null;
        if (w != null && w.done != null) w.done.done(false);
        while (!outbox.isEmpty()) {
            Out o = outbox.pollFirst();
            if (o.done != null) o.done.done(false);
        }
        connectedAtMs = -1;
        events.changed();
    }

    // ---- while connected -----------------------------------------------------
    private void tick(final int e) {
        if (e != epoch || step != Step.READY) return;
        long now = timers.nowMs();
        rx.poll(now);
        alive(false);
        // A message lost on the way (reassembly drop) leaves its record on the
        // Gateway; asking for everything again now and then finds it.
        if (now - lastDumpMs >= DUMP_EVERY_MS) redump();
        // Fast connection while records flow; normal pace once it has been quiet.
        if (high && writing == null && outbox.isEmpty() && core.pendingAccepts() == 0 && now - lastBusyMs >= QUIET_MS) {
            high = false;
            radio.priority(false);
        }
        pump();
        timers.after(TICK_MS, () -> tick(e));
    }

    private void alive(boolean force) {
        long now = timers.nowMs();
        if (!force && now - lastAliveMs < ALIVE_MS) return;
        lastAliveMs = now;
        try { sessions.alive(now / 1000); } catch (Exception ignored) { }
    }

    void onFrame(byte[] frame) {
        // An indication can only come once they are on: take it as the answer
        // to enabling them if that answer has not been seen yet.
        if (step == Step.ENABLING) onEnabled(true);
        if (step != Step.READY) return;
        long now = timers.nowMs();
        byte[] msg = rx.push(frame, now);
        if (msg == null) return;
        if (core.gatewayId == null && msg[0] != 0x01 && msg[0] != EdgeWire.LORA_LINE_TO_EDGE) {
            // The Gateway says HELLO once per connection. Without it no record
            // can be taken (never guess the Gateway): connect again.
            events.log("[BLE] gateway'in ilk mesajı (HELLO) gelmedi; yeniden bağlanılıyor");
            onDisconnected(-2);
            return;
        }
        if (msg[0] == EdgeWire.LORA_LINE_TO_EDGE) {
            if (msg.length > 1) events.loraLine(new String(msg, 1, msg.length - 1, StandardCharsets.US_ASCII));
            return;
        }
        lastBusyMs = now;
        int storedBefore = core.counts[0];
        String line = core.handleMessage(msg);
        if (line != null) events.log(line);
        if (msg[0] == 0x01 && core.gatewayId != null) events.hello(BridgeCore.gatewayHex(core.gatewayId));
        int stored = core.counts[0] - storedBefore;
        if (stored > 0) events.stored(stored);
        if (msg[0] == 0x03) {
            if (core.lastDumpComplete) incompleteRuns = 0;
            else if (++incompleteRuns <= INCOMPLETE_REDUMPS) {
                final int e = epoch;
                timers.after(INCOMPLETE_REDUMP_MS, () -> { if (e == epoch) redump(); });
            }
        }
        alive(false);
        events.changed();
        pump();
    }

    /** Ask the Gateway for every record it holds again (new keys arrived). */
    void redump() {
        if (step != Step.READY) return;
        core.beginDump();
        lastBusyMs = timers.nowMs();
        lastDumpMs = lastBusyMs;
        if (!high) { high = true; radio.priority(true); }
        outbox.addFirst(new Out(EdgeWire.DUMP, null));
        pump();
    }

    /** 6c: one "APP LORA ..." line to the Gateway. done(false) if not connected or not written. */
    void sendLine(String line, Done done) {
        byte[] text = line.getBytes(StandardCharsets.US_ASCII);
        if (step != Step.READY || text.length == 0 || text.length + 1 > EdgeWire.MAX_MESSAGE) { done.done(false); return; }
        byte[] msg = new byte[text.length + 1];
        msg[0] = LORA_LINE_TO_GATEWAY;
        System.arraycopy(text, 0, msg, 1, text.length);
        outbox.addLast(new Out(msg, done));
        pump();
    }

    private void pump() {
        if (step != Step.READY || writing != null) return;
        Out next = outbox.pollFirst();
        if (next == null) {
            byte[] accept = core.nextAcceptMessage(timers.nowMs());
            if (accept == null) return;
            next = new Out(accept, null);
        }
        writing = next;
        messageId = (messageId + 1) & 0xff;
        writingFrames = EdgeWire.frames(next.message, messageId);
        writingIndex = 0;
        busyTries = 0;
        writeFrame();
    }

    private void writeFrame() {
        if (writing == null || step != Step.READY) return;
        final int e = epoch, token = ++writeToken;
        if (!radio.write(writingFrames.get(writingIndex))) {
            // Android allows one GATT operation at a time: try again shortly.
            if (++busyTries > WRITE_BUSY_TRIES) { finish(false); return; }
            timers.after(WRITE_BUSY_MS, () -> { if (e == epoch && token == writeToken) writeFrame(); });
            return;
        }
        timers.after(WRITE_TIMEOUT_MS, () -> {
            if (e == epoch && token == writeToken && step == Step.READY) {
                events.log("[BLE] yazma cevabı gelmedi; bağlantı yenileniyor");
                onDisconnected(-1);
            }
        });
    }

    void onWritten(boolean ok) {
        if (writing == null || step != Step.READY) return;
        writeToken++;
        if (!ok) {
            // An ACCEPT stays in flight in the core and is sent again after 3 s.
            events.log("[BLE] yazılamadı");
            finish(false);
            return;
        }
        if (++writingIndex < writingFrames.size()) { busyTries = 0; writeFrame(); return; }
        finish(true);
    }

    private void finish(boolean ok) {
        Out w = writing;
        writing = null;
        if (w != null && w.done != null) w.done.done(ok);
        pump();
    }
}
