package tr.orun.edge;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Which of the server's herd alerts become a phone notification, and what to
 * remember afterwards. Pure Java (no Android types) so it is compiled and
 * tested on the development PC: tools/orun_android_mantik_test.sh.
 *
 * The decision "is this animal outside / silent / low on battery" is made by
 * the account server (backend/herd_alerts.py). This class only makes sure
 * every alert is shown once:
 *  - first run: nothing is shown, what is already there is remembered;
 *  - an alert is shown when its key was not seen before;
 *  - "alana döndü" (needs = the key of the leaving) only when that leaving
 *    was itself shown or remembered.
 * Same rule as the app's src/app/alerts.ts nextAlerts.
 */
final class HerdWatchLogic {
    static final int KEEP = 400;

    static final class Offer {
        final String key, title, body, needs;
        Offer(String key, String title, String body, String needs) {
            this.key = key; this.title = title; this.body = body; this.needs = needs;
        }
    }

    static final class Outcome {
        final List<Offer> show = new ArrayList<>();
        final List<String> keys = new ArrayList<>();
    }

    private HerdWatchLogic() { }

    static Outcome decide(List<Offer> offers, boolean ready, List<String> known) {
        Outcome out = new Outcome();
        Set<String> seen = new HashSet<>(known);
        out.keys.addAll(known);
        for (Offer o : offers) {
            if (o == null || o.key == null || o.key.isEmpty() || seen.contains(o.key)) continue;
            if (ready && (o.needs == null || o.needs.isEmpty() || seen.contains(o.needs)))
                out.show.add(o);
            out.keys.add(o.key);
        }
        // An alert shown in this round counts for a "back" in the same round
        // only from the next poll on: order of the server's list must not matter.
        while (out.keys.size() > KEEP) out.keys.remove(0);
        return out;
    }

    /** Stable positive notification id for an alert key. */
    static int idOf(String key) {
        int h = 0x811c9dc5;
        for (int i = 0; i < key.length(); i++) h = (h ^ key.charAt(i)) * 16777619;
        int id = h >>> 1;
        return id == 0 ? 1 : id;
    }

    /** "02:31" style minutes for the "server unreachable" text. */
    static String minutes(long ms) {
        long m = Math.max(1, ms / 60000L);
        return m < 120 ? m + " dakikadır" : (m / 60) + " saattir";
    }
}
