import assert from "node:assert/strict";
import { test } from "node:test";
import { OfflineSyncStore, zoneBody, type SyncStorage, type SyncOp } from "../src/app/offlineSync.ts";
import {
  SharedSyncClient, SharedSyncError, sharedSyncOrigin, type EnrolledSyncSession,
  type SyncFetch,
} from "../src/app/sharedSyncClient.ts";
import type { Zone } from "../src/app/zones.ts";

const RING: Zone["ring"] = [[28.054,37.610],[28.055,37.610],[28.055,37.611],[28.054,37.611]];
const ZONE: Zone = { id:"z1", name:"Kuzey", ring:RING, active:true, updated:0 };
const T_A = "A".repeat(64), T_B = "B".repeat(64);
const auth = (token: string, site="site-a", installation="phone-a"): EnrolledSyncSession =>
  ({ site, installation, accessToken:token });
const json = (body:unknown,status=200):Response => new Response(JSON.stringify(body),{
  status, headers:{"Content-Type":"application/json"},
});
function local(site="site-a",install="phone-a") {
  const map=new Map<string,string>();
  const storage: SyncStorage={
    getItem:(key)=>map.get(key)??null,
    setItem:(key,v)=>{map.set(key,v);},
  };
  const db=new OfflineSyncStore(storage, "test-shared", site, install);
  return {db,map};
}
function stageOne(db: OfflineSyncStore, opId="op1") {
  db.stageLegacy([ZONE],{groups:[],rules:{}},()=>opId);
}
function reqHeader(init:RequestInit,name:string):string {
  return new Headers(init.headers).get(name)??"";
}
function expectedFailure(reason: string) {
  return (err:unknown):boolean => err instanceof SharedSyncError && err.reason===reason;
}

test("only HTTPS remote origin, loopback allowed for isolated host; never URL credentials",()=>{
  assert.equal(sharedSyncOrigin("https://orun.example/"),"https://orun.example");
  assert.equal(sharedSyncOrigin("http://127.0.0.1:8098/"),"http://127.0.0.1:8098");
  for(const url of [
    "http://100.80.90.10:8090/","https://u:p@orun.example/","https://orun.example/prefix",
    "https://orun.example/?token=secret","ftp://orun.example/","https://orun.example/#x",
    "http://orun.example/","not-a-url",
  ]) assert.throws(()=>sharedSyncOrigin(url),expectedFailure("protocol"));
});

test("enrolled phone uploads a durable operation, another phone pulls exact E7",async()=>{
  const alice=local("site-a","phone-a"), bob=local("site-a","phone-b");
  stageOne(alice.db);
  const calls:{url:string;init:RequestInit}[]=[];
  const remote: {sequence:number;body:Record<string,unknown>}[]=[];
  const fetcher: SyncFetch=async(url,init)=>{
    calls.push({url,init});
    const token=reqHeader(init,"authorization").slice(7);
    assert.ok([T_A,T_B].includes(token));
    assert.equal(init.credentials,"omit");
    assert.equal(init.cache,"no-store");
    assert.equal(init.redirect,"manual");
    assert.equal(url.includes(token),false);
    if(url.endsWith("/api/v1/sync/ops")){
      assert.equal(token,T_A);
      const data=JSON.parse(String(init.body)) as {ops:SyncOp[]};
      const op=data.ops[0];
      remote.push({sequence:remote.length+1,body:op.body!});
      return json({results:[{op_id:op.op_id,status:"ACCEPTED",
        revision:1,sequence:remote.length}]});
    }
    if(url.includes("/api/v1/sync?")){
      const cursor=Number(new URL(url).searchParams.get("cursor"));
      return json({changes:remote.filter(x=>x.sequence>cursor).map(x=>({
        sequence:x.sequence,kind:"zone",id:"z1",revision:1,
        body:x.body,deleted:false,
      })),cursor:remote.length||cursor,has_more:false});
    }
    return json({error:"bad"},404);
  };
  const a=new SharedSyncClient(alice.db,"https://orun.example/",()=>auth(T_A),fetcher);
  const b=new SharedSyncClient(bob.db,"https://orun.example/",()=>auth(T_B,"site-a","phone-b"),fetcher);
  const ra=await a.runOnce();
  assert.equal(ra.accepted,1);
  assert.equal(ra.pending,0);
  const rb=await b.runOnce();
  assert.equal(rb.received,1);
  assert.equal(rb.pending,0);
  assert.equal(rb.moreAvailable,false);
  assert.deepEqual(bob.db.get().resources["zone:z1"].body?.ring_e7,zoneBody(ZONE).ring_e7);
  assert.equal(calls.length,3);
  assert.ok(calls.every(x=>x.url.startsWith("https://orun.example/")));
});

test("without enrolled session or with different installation, no network and no data loss",async()=>{
  const {db}=local();
  stageOne(db);
  let called=0;
  const fetcher:SyncFetch=async()=>{called++;return json({});};
  await assert.rejects(
    new SharedSyncClient(db,"https://orun.example/",()=>null,fetcher).runOnce(),
    expectedFailure("not_enrolled"));
  await assert.rejects(
    new SharedSyncClient(db,"https://orun.example/",()=>auth(T_A,"site-b"),fetcher).runOnce(),
    expectedFailure("wrong_installation"));
  await assert.rejects(
    new SharedSyncClient(db,"https://orun.example/",()=>auth(T_A,"site-a","phone-b"),fetcher).runOnce(),
    expectedFailure("wrong_installation"));
  assert.equal(called,0);
  assert.equal(db.nextBatch().length,1);
});

test("server 401, 403 and missing network preserve pending ops",async()=>{
  for(const [status,reason] of [[401,"auth_expired"],[403,"forbidden"]] as const){
    const {db}=local();
    stageOne(db);
    const client=new SharedSyncClient(db,"https://orun.example/",()=>auth(T_A),
      async()=>json({error:"reject"},status));
    await assert.rejects(client.runOnce(),expectedFailure(reason));
    assert.equal(db.nextBatch().length,1);
  }
  const {db}=local();
  stageOne(db);
  const client=new SharedSyncClient(db,"https://orun.example/",()=>auth(T_A),
    async()=>{throw Error("network lost, not a backend ACCEPT");});
  await assert.rejects(client.runOnce(),expectedFailure("server_unavailable"));
  assert.equal(db.nextBatch().length,1);
});

test("redirect, malformed response, ACK mismatch and invalid pull do not advance state",async()=>{
  const cases: {res:Response;reason:string}[]=[
    {res:json({no_results:1}),reason:"protocol"},
    {res:json({results:[{op_id:"unknown",status:"ACCEPTED",revision:1,sequence:1}]}),reason:"protocol"},
    {res:json({results:[]}),reason:"protocol"},
    {res:json({error:"redirect"},302),reason:"protocol"},
    {res:new Response("hello",{status:200,headers:{"Content-Type":"text/plain"}}),reason:"protocol"},
  ];
  for(const {res,reason} of cases){
    const {db}=local();
    stageOne(db);
    await assert.rejects(new SharedSyncClient(db,"https://orun.example/",
      ()=>auth(T_A),async()=>res).runOnce(),expectedFailure(reason));
    assert.equal(db.nextBatch().length,1);
  }
  const {db}=local();
  const c=new SharedSyncClient(db,"https://orun.example/",()=>auth(T_A),
    async()=>json({changes:[],cursor:99,has_more:false}));
  await assert.rejects(c.runOnce(),expectedFailure("protocol"));
  assert.equal(db.get().cursor,0);
});

test("server accepted but local persistence failure does not lose pending op",async()=>{
  const map=new Map<string,string>();
  let fail=false;
  const storage:SyncStorage={
    getItem:key=>map.get(key)??null,
    setItem:(key,val)=>{if(fail)throw Error("full");map.set(key,val);},
  };
  const db=new OfflineSyncStore(storage,"shared","site-a","phone-a");
  stageOne(db);
  fail=true;
  const client=new SharedSyncClient(db,"https://orun.example/",()=>auth(T_A),
    async()=>json({results:[{op_id:"op1",status:"ACCEPTED",revision:1,sequence:1}]}));
  await assert.rejects(client.runOnce(),expectedFailure("local_storage"));
  assert.equal(db.nextBatch().length,1);
  assert.equal(new OfflineSyncStore(storage,"shared","site-a","phone-a").nextBatch().length,1);
});

test("same run cannot overlap: no duplicate concurrent HTTP operations",async()=>{
  const {db}=local(); stageOne(db);
  let release!:()=>void;
  const hold=new Promise<void>(resolve=>{release=resolve;});
  let entering!:()=>void;
  const entered=new Promise<void>(resolve=>{entering=resolve;});
  const client=new SharedSyncClient(db,"https://orun.example/",()=>auth(T_A),
    async(_url,_init)=>{
      entering();
      await hold;
      return json({results:[{op_id:"op1",status:"ACCEPTED",revision:1,sequence:1}]});
    });
  const first=client.runOnce();
  await entered;
  await assert.rejects(client.runOnce(),expectedFailure("busy"));
  release();
  // First response is accepted, then same fake response is invalid as GET:
  // progress of first call still persisted, but cursor stays unchanged.
  await assert.rejects(first,expectedFailure("protocol"));
  assert.equal(db.nextBatch().length,0);
  assert.equal(db.get().cursor,0);
});

test("bounded paginated pull; server tombstone persists and never changes local draft",async()=>{
  const {db,map}=local();
  map.set("orun.app.zones.v1","ORIGINAL");
  let reads=0;
  const client=new SharedSyncClient(db,"https://orun.example/",()=>auth(T_A),
    async(url)=>{
      reads++;
      const cursor=Number(new URL(url).searchParams.get("cursor"));
      if(cursor===0) return json({changes:[{
        sequence:10,kind:"zone",id:"z1",revision:1,body:zoneBody(ZONE),deleted:false,
      }],cursor:10,has_more:true});
      if(cursor===10) return json({changes:[{
        sequence:12,kind:"zone",id:"z1",revision:2,body:null,deleted:true,
      }],cursor:12,has_more:false});
      return json({changes:[],cursor,has_more:false});
    });
  const r=await client.runOnce();
  assert.equal(r.received,2);
  assert.equal(r.moreAvailable,false);
  assert.equal(reads,2);
  assert.equal(db.get().cursor,12);
  assert.equal(db.get().resources["zone:z1"].deleted,true);
  assert.equal(map.get("orun.app.zones.v1"),"ORIGINAL");
});

test("storage corruption blocks all HTTP requests, not bypassed by valid token",async()=>{
  const map=new Map<string,string>([["shared","{bad"]]);
  const storage:SyncStorage={getItem:k=>map.get(k)??null,setItem:(k,v)=>{map.set(k,v);}};
  const db=new OfflineSyncStore(storage,"shared","site-a","phone-a");
  let calls=0;
  const client=new SharedSyncClient(db,"https://orun.example/",()=>auth(T_A),async()=>{
    calls++;
    return json({});
  });
  await assert.rejects(client.runOnce(),expectedFailure("local_storage"));
  assert.equal(calls,0);
  assert.equal(map.get("shared"),"{bad");
});
