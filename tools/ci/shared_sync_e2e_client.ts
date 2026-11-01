// ORUN S3C: true Node fetch against the Python SharedStore + Step3C HTTP
// server, not an injected fake fetch. Runner launches the server on loopback
// and supplies ephemeral, independently enrolled test credentials.
import assert from "node:assert/strict";
import { OfflineSyncStore, groupBody, zoneBody, type SyncStorage } from "../../edge/app/src/app/offlineSync.ts";
import { SharedSyncClient } from "../../edge/app/src/app/sharedSyncClient.ts";
import type { Zone } from "../../edge/app/src/app/zones.ts";
import type { TargetBook } from "../../edge/app/src/app/zoneTargets.ts";

const endpoint=process.env.ORUN_SYNC_E2E_URL ?? "";
const tokens={
  a:process.env.ORUN_SYNC_E2E_A ?? "",
  b:process.env.ORUN_SYNC_E2E_B ?? "",
  c:process.env.ORUN_SYNC_E2E_C ?? "",
  d:process.env.ORUN_SYNC_E2E_D ?? "",
};
if (!endpoint.startsWith("http://127.0.0.1:") ||
    Object.values(tokens).some((v)=>v.length<48)) {
  throw new Error("Requires ephemeral, loopback-only enrollment test fixture.");
}
const SQ:Zone["ring"]=[[28.054,37.610],[28.055,37.610],
  [28.055,37.611],[28.054,37.611]];
const zone:Zone={id:"z1",name:"Kuzey",ring:SQ,active:true,updated:0};
const targets:TargetBook={
  groups:[{id:"g1",name:"Kuzey sürüsü",devices:["T-01","T-02"]}],
  rules:{z1:{scope:"selected",devices:["T-01"],groups:["g1"]}},
};
function local(site:string,installation:string) {
  const disk=new Map<string,string>();
  const storage:SyncStorage={
    getItem:(key)=>disk.get(key)??null,
    setItem:(key,value)=>{disk.set(key,value);},
  };
  const make=()=>new OfflineSyncStore(storage,"e2e-mirror",site,installation);
  return {disk,make,store:make()};
}
function client(store:OfflineSyncStore,site:string,installation:string,token:string) {
  return new SharedSyncClient(store,endpoint,
    ()=>({site,installation,accessToken:token}));
}
const a=local("owner-site","phone-a"),b=local("owner-site","phone-b"),
  c=local("other-site","phone-c"),d=local("owner-site","pc-editor");
const aClient=client(a.store,"owner-site","phone-a",tokens.a);
const bClient=client(b.store,"owner-site","phone-b",tokens.b);
const cClient=client(c.store,"other-site","phone-c",tokens.c);
const dClient=client(d.store,"owner-site","pc-editor",tokens.d);

// The original area and targets on A are never modified by migration.
a.disk.set("orun.app.zones.v1","ORIGINAL-ZONE-DOCUMENT");
a.disk.set("orun.app.zoneTargets.v1","ORIGINAL-TARGET-DOCUMENT");
a.store.stageLegacy([zone],targets,(()=>{
  let n=0;
  return ()=>["migration-zone","migration-group","migration-rule"][n++]!;
})());
assert.equal(a.store.nextBatch().length,3);
const sent=await aClient.runOnce();
assert.equal(sent.accepted,3);
assert.equal(sent.pending,0);
assert.equal(sent.needsReview,0);
assert.equal(a.disk.get("orun.app.zones.v1"),"ORIGINAL-ZONE-DOCUMENT");
assert.equal(a.disk.get("orun.app.zoneTargets.v1"),"ORIGINAL-TARGET-DOCUMENT");

// A separate, READ-ONLY enrolled phone receives precisely A's revisions.
const fromB=await bClient.runOnce();
assert.equal(fromB.received,3);
assert.equal(fromB.pending,0);
assert.deepEqual(b.store.get().resources["zone:z1"].body?.ring_e7,zoneBody(zone).ring_e7);
assert.deepEqual(b.store.get().resources["group:g1"].body,groupBody(targets.groups[0]));
assert.equal(b.store.get().resources["zone_target:z1"].body?.zone_id,"z1");
assert.equal(b.store.get().resources["zone:z1"].revision,1);
assert.equal(b.make().get().resources["zone:z1"].revision,1,
  "freshly opened second client mirror must retain received state");
assert.equal((await bClient.runOnce()).received,0,"no duplicate pull");

// Different site cannot observe owner-site data even when IDs collide.
assert.equal((await cClient.runOnce()).received,0);
c.store.stageLegacy([{...zone,name:"Other site"}],
  {groups:[],rules:{}},()=> "other-zone");
assert.equal((await cClient.runOnce()).accepted,1);
assert.equal(c.store.get().resources["zone:z1"].body?.name,"Other site");
assert.equal(b.store.get().resources["zone:z1"].body?.name,"Kuzey");

// Another authorized editor catches up, then edits from an old revision.
assert.equal((await dClient.runOnce()).received,3);
const aNew={...zone,name:"Güney"};
a.store.queue("zone","z1",zoneBody(aNew),"update-owner");
assert.equal((await aClient.runOnce()).accepted,1);
assert.equal((await bClient.runOnce()).received,1);
assert.equal(b.store.get().resources["zone:z1"].body?.name,"Güney");
d.store.queue("zone","z1",zoneBody({...zone,name:"Stale PC"}),"stale-edit");
const outdated=await dClient.runOnce();
assert.equal(outdated.conflicts,1);
assert.equal(outdated.needsReview,1);
assert.equal(d.store.get().problems[0].op.body?.name,"Stale PC");
assert.equal(d.store.get().resources["zone:z1"].body?.name,"Güney",
  "stale edit cannot silently overwrite canonical server version");

// Read-only phone cannot submit a mutation; rejection preserves the edit
// in the review queue and does not grant Tracker command authority.
b.store.queue("zone","z1",zoneBody({...zone,name:"Viewer tried edit"}),"viewer-edit");
const denied=await bClient.runOnce();
assert.equal(denied.rejected,1);
assert.equal(denied.needsReview,1);
assert.equal(b.store.get().resources["zone:z1"].body?.name,"Güney");
assert.equal("applied" in b.store.get().resources["zone:z1"],false);

process.stdout.write("SHARED_SYNC_REAL_HTTP_E2E: PASS - owner upload 3/3, viewer pull 3/3, ");
process.stdout.write("cross-site isolated, revision conflict retained, viewer write rejected, restart retained\n");
