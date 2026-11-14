package tr.orun.edge;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** PC check of HerdWatchLogic (the only Android-side rule). Run: bash tools/orun_android_mantik_test.sh */
public final class HerdWatchLogicCheck {
    private static int failed = 0;

    private static void check(String what, boolean ok) {
        System.out.println((ok ? "PASS " : "FAIL ") + what);
        if (!ok) failed++;
    }

    private static HerdWatchLogic.Offer o(String key, String needs) {
        return new HerdWatchLogic.Offer(key, "t:" + key, "b", needs);
    }

    public static void main(String[] args) {
        List<String> none = new ArrayList<>();
        List<HerdWatchLogic.Offer> two = Arrays.asList(o("out:A:1", null), o("battery:A:9", null));

        HerdWatchLogic.Outcome first = HerdWatchLogic.decide(two, false, none);
        check("first run shows nothing", first.show.isEmpty());
        check("first run remembers what is there", first.keys.equals(Arrays.asList("out:A:1", "battery:A:9")));

        HerdWatchLogic.Outcome fresh = HerdWatchLogic.decide(two, true, none);
        check("each new alert is shown", fresh.show.size() == 2);
        check("nothing is shown twice", HerdWatchLogic.decide(two, true, fresh.keys).show.isEmpty());

        List<HerdWatchLogic.Offer> back = Arrays.asList(o("back:out:A:1", "out:A:1"));
        check("back is shown when the leaving was told", HerdWatchLogic.decide(back, true, fresh.keys).show.size() == 1);
        HerdWatchLogic.Outcome orphan = HerdWatchLogic.decide(back, true, none);
        check("back without a told leaving is silent", orphan.show.isEmpty());
        check("and is not examined again", orphan.keys.contains("back:out:A:1"));

        List<String> many = new ArrayList<>();
        for (int i = 0; i < 1000; i++) many.add("k" + i);
        HerdWatchLogic.Outcome capped = HerdWatchLogic.decide(Arrays.asList(o("new", null)), true, many);
        check("memory is bounded", capped.keys.size() == HerdWatchLogic.KEEP);
        check("newest keys are kept", capped.keys.get(capped.keys.size() - 1).equals("new"));

        check("notification ids are stable and positive",
                HerdWatchLogic.idOf("out:A:1") == HerdWatchLogic.idOf("out:A:1") && HerdWatchLogic.idOf("out:A:1") > 0
                        && HerdWatchLogic.idOf("out:A:1") != HerdWatchLogic.idOf("out:A:2"));
        check("blind time wording", HerdWatchLogic.minutes(31 * 60000L).equals("31 dakikadır")
                && HerdWatchLogic.minutes(3 * 3600000L).equals("3 saattir"));

        System.out.println(failed == 0 ? "RESULT PASS" : "RESULT FAIL (" + failed + ")");
        if (failed != 0) System.exit(1);
    }
}
