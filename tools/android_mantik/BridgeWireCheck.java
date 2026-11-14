package tr.orun.edge;

import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

/**
 * PC check of the native bridge's protocol code (EdgeWire, ProductFrame):
 * every line of bridge_vectors.txt was produced by the TypeScript original;
 * the Java copy must give exactly the same answer.
 *   bash tools/orun_android_mantik_test.sh
 */
public final class BridgeWireCheck {
    private static int failed = 0, checked = 0;

    static byte[] hex(String s) {
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return b;
    }

    static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte v : b) sb.append(String.format("%02X", v & 0xff));
        return sb.toString();
    }

    static void same(String what, String want, String got) {
        checked++;
        if (!want.equals(got)) {
            failed++;
            if (failed <= 10) System.out.println("FAIL " + what + "\n  want " + want + "\n  got  " + got);
        }
    }

    public static void main(String[] args) throws Exception {
        for (String line : Files.readAllLines(Paths.get(args[0]))) {
            if (line.startsWith("#") || line.isEmpty()) continue;
            String[] f = line.split("\\|", -1);
            switch (f[0]) {
                case "ccm": {
                    byte[] out = ProductFrame.ccmDecrypt(hex(f[1]), hex(f[2]), hex(f[4]), hex(f[3]), 8);
                    same("ccm", f[5], out == null ? "NULL" : hex(out));
                    break;
                }
                case "frames": {
                    List<String> got = new ArrayList<>();
                    for (byte[] fr : EdgeWire.frames(hex(f[2]), Integer.parseInt(f[1]))) got.add(hex(fr));
                    same("frames", f[3], String.join(",", got));
                    break;
                }
                case "reassemble": {
                    EdgeWire.Reassembler r = new EdgeWire.Reassembler();
                    List<String> got = new ArrayList<>();
                    if (!f[1].isEmpty())
                        for (String fr : f[1].split(",")) { byte[] o = r.push(hex(fr), 0); got.add(o == null ? "-" : hex(o)); }
                    same("reassemble", f[2], String.join(",", got));
                    break;
                }
                case "mac": {
                    byte[] mac = EdgeWire.acceptMac(Long.parseUnsignedLong(f[1], 16), Integer.parseInt(f[2]),
                            Integer.parseInt(f[3]), Long.parseUnsignedLong(f[4], 16), hex(f[5]), hex(f[6]));
                    same("mac", f[7], hex(mac));
                    break;
                }
                case "msg": {
                    EdgeWire.Message d = EdgeWire.decode(hex(f[1]));
                    String text = "NULL";
                    if (d != null) {
                        switch (d.type) {
                            case EdgeWire.HELLO:
                                text = String.format("hello:%016X:%d:%d", d.gateway, d.held, d.capacity); break;
                            case EdgeWire.HELD: case EdgeWire.HELD_PREVIEW:
                                text = (d.type == EdgeWire.HELD ? "held" : "heldPreview") +
                                        String.format(":%d:%d:%016X:", d.page, d.slot, d.generation) +
                                        (d.hasLink ? d.rssi + ":" + d.snr : "nolink") + ":" + hex(d.object);
                                break;
                            case EdgeWire.DUMP_END: text = "dumpEnd:" + d.count; break;
                            case EdgeWire.ACCEPT_RESULT: text = "acceptResult:" + d.page + ":" + d.slot + ":" + d.code; break;
                            default: text = "reclaimed:" + d.page + ":" + (d.ok ? 1 : 0);
                        }
                    }
                    same("msg " + f[1], f[2], text);
                    break;
                }
                case "open": {
                    final long epoch = Long.parseLong(f[1]);
                    final byte[] key = hex(f[2]);
                    ProductFrame.Opened o = ProductFrame.open(hex(f[3]), (dev, e) -> e == epoch ? key : null);
                    String got = o == null ? "REJECT" : "OK:" + hex(o.device) + ":" + hex(o.incarnation) + ":" +
                            o.sequence + ":" + o.family + ":" + hex(o.plaintext);
                    same("open " + f[3].substring(0, 16), f[4], got);
                    break;
                }
                default:
                    failed++;
                    System.out.println("FAIL unknown vector " + f[0]);
            }
        }
        System.out.println("BRIDGE " + (checked - failed) + "/" + checked + (failed == 0 ? " PASS" : " FAIL"));
        if (failed != 0) System.exit(1);
    }
}
