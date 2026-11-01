import assert from "node:assert/strict";
import { test } from "node:test";
import { blankTargets, emptyRule, targetsForZone, zonePlanForDevice, ZoneTargetStore, type TargetBook } from "../src/app/zoneTargets.ts";
import type { Zone } from "../src/app/zones.ts";

const ring: Zone["ring"] = [[28,37],[28.001,37],[28.001,37.001],[28,37.001]];
const z = (id: string): Zone => ({ id, name:id, ring, active:true, updated:0 });
const known = ["tracker-1", "tracker-2", "tracker-3"];

test("no target is ever implicit, including existing saved zones", () => {
  const b = blankTargets();
  assert.deepEqual(targetsForZone(b, "z1", known), []);
  assert.deepEqual(zonePlanForDevice(b, [z("z1")], "tracker-1", known).zones, []);
  assert.deepEqual(emptyRule(), { scope: "selected", devices:[], groups:[] });
});
test("explicit device and group targets are resolved only against known IDs", () => {
  const b: TargetBook = { groups:[{ id:"g1", name:"Kuzey", devices:["tracker-2", "stale"] }],
    rules: { z1: { scope:"selected", devices:["tracker-1"], groups:["g1"] } } };
  assert.deepEqual(targetsForZone(b, "z1", known), ["tracker-1","tracker-2"]);
  assert.deepEqual(zonePlanForDevice(b, [z("z1")], "tracker-3", known).zones, []);
  assert.deepEqual(targetsForZone({ ...b, rules:{ z1:{ scope:"all", devices:[], groups:[] } } }, "z1", known), known);
});
test("full replacement geometry is derived separately for each device", () => {
  const b: TargetBook = { groups:[], rules: {
    z1: { scope:"selected", devices:["tracker-1"],groups:[] },
    z2: { scope:"selected", devices:["tracker-2"],groups:[] },
  }};
  assert.deepEqual(zonePlanForDevice(b, [z("z1"),z("z2")], "tracker-1", known).zones.map(x=>x.id), ["z1"]);
  assert.deepEqual(zonePlanForDevice(b, [z("z1"),z("z2")], "tracker-2", known).zones.map(x=>x.id), ["z2"]);
});
test("storage writes precede success; corrupt existing target plans are protected", () => {
  const data = new Map<string,string>();
  const prev = Object.getOwnPropertyDescriptor(globalThis, "localStorage");
  Object.defineProperty(globalThis, "localStorage", { configurable:true, value:{
    getItem:(key:string)=>data.get(key)??null,
    setItem:(key:string,v:string)=>{ data.set(key,v); },
  }});
  try {
    const store = new ZoneTargetStore("targets");
    let changed=0;
    store.subscribe(()=>changed++);
    store.saveGroup({ id:"g1",name:"Kuzey",devices:["tracker-1","tracker-2"] });
    store.saveRule("z1", { scope:"selected",devices:[],groups:["g1"] });
    assert.equal(changed,2);
    assert.deepEqual(targetsForZone(new ZoneTargetStore("targets").get(),"z1",known), ["tracker-1","tracker-2"]);
    store.removeGroup("g1");
    assert.deepEqual(targetsForZone(store.get(),"z1",known), []);
    data.set("corrupt","{invalid");
    const corrupt = new ZoneTargetStore("corrupt");
    assert.ok(corrupt.warning);
    assert.throws(()=>corrupt.saveRule("z1",{scope:"all",devices:[],groups:[]}));
    assert.equal(data.get("corrupt"),"{invalid");
    Object.defineProperty(globalThis, "localStorage",{ configurable:true, value:{
      getItem:(key:string)=>data.get(key)??null, setItem:()=>{throw new Error("quota");},
    }});
    const blocked = new ZoneTargetStore("writable");
    assert.throws(()=>blocked.saveRule("z1",{scope:"all",devices:[],groups:[]}),/quota/);
    assert.deepEqual(blocked.get(),blankTargets());
  } finally {
    if (prev) Object.defineProperty(globalThis, "localStorage",prev);
    else delete (globalThis as {localStorage?:unknown}).localStorage;
  }
});
