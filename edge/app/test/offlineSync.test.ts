import assert from "node:assert/strict";
import { test } from "node:test";
import { OfflineSyncStore, groupBody, targetBody, zoneBody, type SyncStorage } from "../src/app/offlineSync.ts";
import { canonicalE7, type Zone } from "../src/app/zones.ts";
import type { TargetBook } from "../src/app/zoneTargets.ts";

const ring: Zone["ring"] = [[28.054,37.610],[28.055,37.610],[28.055,37.611],[28.054,37.611]];
const zone = (id="z1"): Zone => ({ id,name:"Kuzey",ring,active:true,updated:Date.now() });
const targets: TargetBook = {
  groups: [{id:"g1",name:"Birinci",devices:["T-02","T-01"]}],
  rules: {z1:{scope:"selected",devices:["T-01"],groups:["g1"]}},
};
function env(key="shared") {
  const data = new Map<string,string>();
  const storage: SyncStorage = {
    getItem: (k) => data.get(k) ?? null,
    setItem: (k,v) => { data.set(k,v); },
  };
  return {data, storage, key, make:(site="site-a",install="phone-a") =>
    new OfflineSyncStore(storage,key,site,install)};
}
const ids = (...values:string[]) => {
  let index=0;
  return () => values[index++] ?? "late-" + index;
};

test("zone/group/target convert to backend E7 and remain draft only",()=>{
  const z = zone();
  assert.deepEqual(zoneBody(z).ring_e7,canonicalE7(ring));
  assert.deepEqual(groupBody(targets.groups[0]).device_ids,["T-01","T-02"]);
  assert.deepEqual(targetBody("z1",targets.rules.z1).group_ids,["g1"]);
  assert.throws(()=>targetBody("z1",{scope:"all",devices:["T-01"],groups:[]}),/Hedef/);
});

test("one-time local staging preserves legacy originals, even after restart",()=>{
  const e=env();
  const oldZone=JSON.stringify([zone()]);
  const oldTarget=JSON.stringify(targets);
  e.data.set("orun.app.zones.v1",oldZone);
  e.data.set("orun.app.zoneTargets.v1",oldTarget);
  const a=e.make();
  a.stageLegacy([zone()],targets,ids("op-z","op-g","op-t"));
  assert.equal(a.get().legacyStaged,true);
  assert.deepEqual(a.nextBatch().map((o)=>o.kind),["zone","group","zone_target"]);
  assert.equal(a.nextBatch()[0].base_revision,0);
  assert.deepEqual(a.nextBatch()[0].body?.ring_e7,canonicalE7(ring));
  assert.equal(e.data.get("orun.app.zones.v1"),oldZone);
  assert.equal(e.data.get("orun.app.zoneTargets.v1"),oldTarget);
  const b=e.make();
  assert.deepEqual(b.nextBatch(),a.nextBatch());
  assert.throws(()=>b.stageLegacy([zone()],targets,ids("different")),/zaten/);
});

test("invalid legacy migration is atomic: no partial queue and originals unchanged",()=>{
  const e=env();
  assert.throws(()=>e.make().stageLegacy([zone()], {
    groups:[],rules:{z1:{scope:"selected",devices:[],groups:["missing-group"]}}
  },ids("q1","q2")),/bulunamayan/);
  assert.equal(e.data.get(e.key),undefined);
  assert.equal(e.make().get().legacyStaged,false);
  const duplicate=zone();
  assert.throws(()=>e.make().stageLegacy([zone(),duplicate],targets,ids("a","b")),/Tekrarlanan/);
});

test("offline durable outbox requires successful storage commit",()=>{
  const e=env();
  let failures=false;
  const storage: SyncStorage={
    getItem:e.storage.getItem,
    setItem:(k,v)=>{if(failures)throw Error("disk full"); e.storage.setItem(k,v);},
  };
  const a=new OfflineSyncStore(storage,e.key,"site-a","phone-a");
  failures=true;
  assert.throws(()=>a.stageLegacy([zone()],targets,ids("a","b","c")),/disk full/);
  assert.equal(a.get().outbox.length,0);
  assert.equal(e.data.has(e.key),false);
  failures=false;
  a.stageLegacy([zone()],targets,ids("a","b","c"));
  assert.equal(a.nextBatch().length,3);
  failures=true;
  assert.throws(()=>a.recordResults([{op_id:"a",status:"ACCEPTED",revision:1,sequence:1}]),/disk full/);
  assert.equal(a.nextBatch().length,3, "ACK cannot be acknowledged before local commit");
});

test("unknown or corrupted server results never erase pending changes",()=>{
  const e=env();
  const a=e.make();
  a.stageLegacy([zone()],{groups:[],rules:{}},ids("a"));
  const failures=[
    [{op_id:"unknown",status:"ACCEPTED",revision:1,sequence:1}],
    [{op_id:"a",status:"ACCEPTED",revision:99,sequence:1}],
    [{op_id:"a",status:"ACCEPTED",revision:1,sequence:0}],
    [{op_id:"a",status:"ACCEPTED",revision:1,sequence:1},
     {op_id:"a",status:"ACCEPTED",revision:1,sequence:1}],
  ];
  for(const x of failures) assert.throws(()=>a.recordResults(x as never));
  assert.equal(a.nextBatch().length,1);
  a.recordResults([{op_id:"a",status:"ACCEPTED",revision:1,sequence:1}]);
  assert.equal(a.nextBatch().length,0);
  assert.equal(e.make().get().resources["zone:z1"].revision,1);
});

test("conflict does not lose local geometry; manual resolution needed",()=>{
  const e=env();
  const a=e.make();
  a.stageLegacy([zone()],{groups:[],rules:{}},ids("a"));
  const first=a.nextBatch()[0];
  a.recordResults([{op_id:first.op_id,status:"CONFLICT",revision:2}]);
  assert.equal(a.nextBatch().length,0);
  assert.equal(a.get().problems.length,1);
  assert.deepEqual(a.get().problems[0].op.body?.ring_e7,canonicalE7(ring));
  assert.throws(()=>a.queue("zone","z1",zoneBody(zone()),"fresh"),/bekliyor/);
});

test("read cursor and resources are persisted together and never applied to local draft",()=>{
  const e=env();
  const a=e.make();
  e.data.set("orun.app.zones.v1","DO-NOT-TOUCH");
  a.applyPull([{
    sequence:3,kind:"zone",id:"z1",revision:1,
    body:zoneBody(zone()),deleted:false,
  }],3);
  assert.equal(a.get().cursor,3);
  assert.equal(e.make().get().resources["zone:z1"].revision,1);
  assert.equal(e.data.get("orun.app.zones.v1"),"DO-NOT-TOUCH");
  assert.throws(()=>a.applyPull([{sequence:2,kind:"zone",id:"z1",revision:2,
    body:zoneBody(zone()),deleted:false}],3));
  assert.equal(a.get().cursor,3);
  a.applyPull([],3);
  assert.equal(a.get().cursor,3);
});

test("corrupt shared mirror is preserved and blocks every write",()=>{
  const e=env();
  e.data.set(e.key,"{BROKEN");
  const a=e.make();
  assert.ok(a.warning);
  assert.throws(()=>a.stageLegacy([zone()],targets,ids("a","b","c")),/okunamadı/);
  assert.equal(e.data.get(e.key),"{BROKEN");
  const fresh=e.make("site-b","phone-b");
  assert.ok(fresh.warning,"wrong site or install must fail closed");
});

test("accepted revision and tombstone are recorded but never mean Tracker applied",()=>{
  const e=env();
  const a=e.make();
  a.stageLegacy([zone()],{groups:[],rules:{}},ids("a"));
  a.recordResults([{op_id:"a",status:"ACCEPTED",revision:1,sequence:1}]);
  a.queue("zone","z1",null,"delete");
  const p=a.nextBatch()[0];
  assert.equal(p.base_revision,1);
  a.recordResults([{op_id:"delete",status:"ACCEPTED",revision:2,sequence:2}]);
  assert.equal(a.get().resources["zone:z1"].deleted,true);
  assert.throws(()=>a.queue("zone","z1",zoneBody(zone()),"revive"),/Silinen/);
  assert.equal("applied" in a.get().resources["zone:z1"],false);
});

test("empty server page cannot skip unseen changes or acknowledge a false cursor",()=>{
  const e=env();
  const a=e.make();
  assert.throws(()=>a.applyPull([],7),/cursor/);
  assert.equal(a.get().cursor,0);
  assert.equal(e.data.get(e.key),undefined);
});

test("same revision compares nested content canonically and rejects actual changes",()=>{
  const e=env();
  const a=e.make();
  const body=zoneBody(zone());
  a.applyPull([{sequence:1,kind:"zone",id:"z1",revision:1,body,deleted:false}],1);
  const reordered={ring_e7:body.ring_e7,active:body.active,name:body.name};
  a.applyPull([{sequence:2,kind:"zone",id:"z1",revision:1,body:reordered,deleted:false}],2);
  assert.equal(a.get().cursor,2);
  const different={...reordered,ring_e7:[[376100000,280540000],[376110000,280540000],[376110000,280550000]]};
  assert.throws(()=>a.applyPull([{sequence:3,kind:"zone",id:"z1",revision:1,
    body:different,deleted:false}],3),/farklı içerik/);
  assert.equal(a.get().cursor,2);
  assert.deepEqual(a.get().resources["zone:z1"].body,body);
});
