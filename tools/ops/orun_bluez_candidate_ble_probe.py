#!/usr/bin/env python3
"""Read-only SF5L6 BLE notification probe; never sends GATT writes or custody ACKs."""
import asyncio
import os
from pathlib import Path
import sys

ROOT = Path(os.environ.get("ORUN_PC_EDGE_REPO", Path(__file__).resolve().parents[2])).resolve()
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from bleak import BleakClient, BleakScanner
from orun_pc_edge import discover_ble_gateway, request_ble_dump
from orun_edge_bench import EDGE_TO_EDGE_UUID, EdgeReassembler

ADDRESS = "C5:DD:01:85:C9:4B"
GATEWAY = "09A462BD4B275BA5"

async def run():
    reassembler = EdgeReassembler()
    good = {"hello": False, "dump": False, "frames": 0}
    done = asyncio.Event()

    def on_data(_, frame):
        good["frames"] += 1
        if good["frames"] <= 4:
            # Log only framing metadata; sealed custody contents remain private.
            print("FRAME", good["frames"], "size", len(frame),
                  "header", bytes(frame[:4]).hex().upper(), flush=True)
        msg = reassembler.push(bytes(frame))
        if msg is None:
            return
        if msg[0] not in (1, 3):
            print("MESSAGE TYPE:", hex(msg[0]), "length:", len(msg), flush=True)
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
                services = client.services
                rx_characteristic = services.get_characteristic(EDGE_TO_EDGE_UUID)
                tx = services.get_characteristic("6f9a1c21-5b3e-4d8a-9e61-2c4b7a0d3e51")
                print("RX PROPERTIES:", rx_characteristic.properties if rx_characteristic else "MISSING", flush=True)
                print("TX PROPERTIES:", tx.properties if tx else "MISSING", flush=True)
                await request_ble_dump(client)
                print("READ-ONLY DUMP REQUEST: SENT", flush=True)
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
