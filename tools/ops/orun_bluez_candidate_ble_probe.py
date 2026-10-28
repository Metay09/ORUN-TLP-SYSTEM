#!/usr/bin/env python3
"""Read-only SF5L6 BLE notification probe; never sends GATT writes or custody ACKs."""
import asyncio
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from bleak import BleakClient, BleakScanner
from orun_pc_edge import discover_ble_gateway
from orun_edge_bench import EDGE_TO_EDGE_UUID, EdgeReassembler

ADDRESS = "C5:DD:01:85:C9:4B"
GATEWAY = "09A462BD4B275BA5"

async def run():
    rx = EdgeReassembler()
    good = {"hello": False, "dump": False, "frames": 0}
    done = asyncio.Event()

    def on_data(_, frame):
        good["frames"] += 1
        msg = rx.push(bytes(frame))
        if msg is None:
            return
        if len(msg) == 13 and msg[0] == 1:
            got = msg[1:9].hex().upper()
            good["hello"] = got == GATEWAY
            print("HELLO:", got, "MATCH:", good["hello"], flush=True)
        if len(msg) == 3 and msg[0] == 3:
            good["dump"] = True
            print("DUMP END:", int.from_bytes(msg[1:3], "big"), flush=True)
        if good["hello"] and good["dump"]:
            done.set()

    try:
        device = await discover_ble_gateway(BleakScanner, ADDRESS)
        print("DISCOVERED:", bool(device), flush=True)
        if device is not None:
            async with BleakClient(device, timeout=15) as client:
                print("CONNECTED:", client.is_connected, flush=True)
                await client.start_notify(EDGE_TO_EDGE_UUID, on_data)
                try:
                    await asyncio.wait_for(done.wait(), timeout=15)
                except asyncio.TimeoutError:
                    print("HELLO/DUMP TIMEOUT", flush=True)
    except Exception as exc:
        print("ERROR:", type(exc).__name__, str(exc), flush=True)

    print("GATT FRAMES:", good["frames"], flush=True)
    print("RESULT:", "PASS" if done.is_set() else "FAIL", flush=True)
    return 0 if done.is_set() else 1

if __name__ == "__main__":
    sys.exit(asyncio.run(run()))
