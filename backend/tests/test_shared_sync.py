"""Pure-host S2 contract checks: no Android/Tracker/Gateway or running backend changes."""
from __future__ import annotations

import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

from backend.shared_sync_store import Actor, SharedStore, SyncForbidden, SyncInvalid
from backend.shared_sync_http import handler_factory

RING = [[376100000, 280500000], [376100000, 280510000],
        [376110000, 280510000], [376110000, 280500000]]


def zone(name="Kuzey", ring=None):
    return {"name": name, "active": True, "ring_e7": RING if ring is None else ring}


def op(op_id="o1", kind="zone", rid="z1", base=0, body=None, action="UPSERT"):
    return {"op_id": op_id, "kind": kind, "id": rid, "base_revision": base,
            "action": action, "body": zone() if body is None and action == "UPSERT" else body}


class SharedSyncTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "shared.sqlite"
        self.store = SharedStore(self.path)
        scopes = frozenset({"zone:read", "zone:write", "group:read", "group:write"})
        self.a = Actor("site-a", "owner", "phone-a", scopes)
        self.b = Actor("site-a", "owner", "phone-b", scopes)
        self.web = Actor("site-a", "owner", "web-pc", frozenset({"zone:read", "group:read"}))
        self.other = Actor("site-b", "other", "phone-c", scopes)

    def tearDown(self):
        self.tmp.cleanup()

    def test_two_phones_pc_converge_and_restart(self):
        first = self.store.mutate(self.a, [op()])[0]
        self.assertEqual(first["status"], "ACCEPTED")
        a = self.store.pull(self.a)
        b = self.store.pull(self.b)
        pc = self.store.pull(self.web)
        self.assertEqual(a["changes"], b["changes"])
        self.assertEqual(pc["changes"][0]["body"]["name"], "Kuzey")
        self.assertEqual(self.store.pull(self.b, b["cursor"])["changes"], [])
        revived = SharedStore(self.path)
        self.assertEqual(revived.pull(self.web)["changes"], pc["changes"])

    def test_idempotent_retries_do_not_duplicate(self):
        a = self.store.mutate(self.a, [op()])[0]
        b = self.store.mutate(self.a, [op()])[0]
        self.assertEqual(a, b)
        self.assertEqual(len(self.store.pull(self.a)["changes"]), 1)
        illegal = self.store.mutate(self.a, [op(body=zone("Different"))])[0]
        self.assertEqual(illegal["status"], "REJECTED")
        self.assertEqual(illegal["reason"], "op_id reuse")
        self.assertEqual(len(self.store.pull(self.a)["changes"]), 1)

    def test_conflict_preserves_first_write_and_retry_stays_conflict(self):
        self.store.mutate(self.a, [op()])
        old_b = op("b1",base=0,body=zone("Telefon B"))
        result = self.store.mutate(self.b, [old_b])[0]
        self.assertEqual(result, {"op_id": "b1", "status": "CONFLICT", "revision": 1})
        self.assertEqual(self.store.mutate(self.b, [old_b])[0], result)
        self.assertEqual(self.store.pull(self.b)["changes"][-1]["body"]["name"], "Kuzey")
        second = self.store.mutate(self.b,[op("b2",base=1,body=zone("Yeni"))])[0]
        self.assertEqual(second["revision"],2)
        self.assertEqual(self.store.pull(self.a)["changes"][-1]["body"]["name"], "Yeni")

    def test_delete_tombstone_prevents_old_offline_resurrection(self):
        self.store.mutate(self.a, [op()])
        result = self.store.mutate(self.a,[op("d1",base=1,body=None,action="DELETE")])[0]
        self.assertEqual(result["status"],"ACCEPTED")
        self.assertEqual(self.store.pull(self.b)["changes"][-1]["deleted"],True)
        self.assertIsNone(self.store.pull(self.b)["changes"][-1]["body"])
        stale = self.store.mutate(self.b,[op("b2",base=1,body=zone("Geri"))])[0]
        self.assertEqual(stale["status"],"CONFLICT")
        fresh = self.store.mutate(self.b,[op("b3",base=2,body=zone("Geri"))])[0]
        self.assertEqual(fresh["status"],"CONFLICT", "deletion must never revive a reused ID")

    def test_acl_and_site_isolation(self):
        self.store.mutate(self.a,[op()])
        self.assertEqual(self.store.pull(self.other)["changes"],[])
        self.assertEqual(self.store.mutate(self.web,[op("w1")])[0]["status"],"REJECTED")
        self.assertEqual(len(self.store.pull(self.a)["changes"]),1)
        self.store.mutate(self.other,[op()])
        self.assertEqual(len(self.store.pull(self.other)["changes"]),1)
        self.assertEqual(len(self.store.pull(self.a)["changes"]),1)
        with self.assertRaises(SyncForbidden):
            self.store.pull(Actor("site-a","guest","guest-pc",frozenset()))

    def test_geometry_capacity_and_holes_fails_closed(self):
        for invalid in (RING[:2], [RING[0],RING[2],RING[1],RING[3]],
                        [[376100000,280500000],[376100000,390500000],
                         [376110000,390500000]], [[True,1],[2,3],[4,5]]):
            self.assertEqual(self.store.mutate(
                self.a,[op("bad",body=zone("K",invalid))])[0]["status"],"REJECTED")
        self.assertEqual(self.store.pull(self.a)["changes"],[])

    def test_groups_and_pagination(self):
        grp={"name":"Birinci", "device_ids":["TRACKER-A","TRACKER-B"]}
        first=self.store.mutate(self.a,[op("g1",kind="group",rid="g-a",body=grp)])[0]
        self.assertEqual(first["status"],"ACCEPTED")
        self.store.mutate(self.a,[op("g2",kind="group",rid="g-b",body=grp)])
        page=self.store.pull(self.b,0,1)
        self.assertEqual(len(page["changes"]),1)
        next_page=self.store.pull(self.b,page["cursor"],1)
        self.assertEqual(len(next_page["changes"]),1)
        self.assertNotEqual(page["cursor"],next_page["cursor"])
        self.assertEqual(self.store.pull(self.web)["changes"][0]["kind"],"group")

    def test_partial_batch_and_untrusted_actor_rejection(self):
        data=self.store.mutate(self.a,[{"junk":True},op("ok")])
        self.assertEqual([x["status"] for x in data],["REJECTED","ACCEPTED"])
        with self.assertRaises(SyncForbidden):
            self.store.pull({"site_id":"site-a"})
        with self.assertRaises(SyncInvalid):
            self.store.mutate(self.a,[])


class SharedHttpTests(SharedSyncTests):
    def setUp(self):
        super().setUp()
        token_map={"test-a":self.a,"test-b":self.b,"test-web":self.web,"test-other":self.other}
        def resolver(headers):
            v=headers.get("Authorization","")
            return token_map.get(v[7:]) if v.startswith("Bearer ") else None
        self.server=ThreadingHTTPServer(("127.0.0.1",0),handler_factory(self.store,resolver))
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        super().tearDown()

    def request(self,method,path,token=None,data=None):
        conn=http.client.HTTPConnection("127.0.0.1",self.server.server_port,timeout=5)
        headers={}
        if token:
            headers["Authorization"]="Bearer "+token
        if data is not None:
            headers["Content-Type"]="application/json"
        conn.request(method,path,body=None if data is None else json.dumps(data),headers=headers)
        response=conn.getresponse()
        body=json.loads(response.read())
        code=response.status
        conn.close()
        return code,body

    def test_real_http_two_clients_and_cross_site_restriction(self):
        code,_=self.request("GET","/api/v1/sync")
        self.assertEqual(code,401)
        code,posted=self.request("POST","/api/v1/sync/ops","test-a",{"ops":[op()]})
        self.assertEqual(code,200)
        self.assertEqual(posted["results"][0]["status"],"ACCEPTED")
        code,read=self.request("GET","/api/v1/sync?cursor=0","test-b")
        self.assertEqual(code,200)
        self.assertEqual(len(read["changes"]),1)
        code,other=self.request("GET","/api/v1/sync?cursor=0","test-other")
        self.assertEqual(code,200)
        self.assertEqual(other["changes"],[])
        code,viewer=self.request("POST","/api/v1/sync/ops","test-web",{"ops":[op("w1")]})
        self.assertEqual(code,200)
        self.assertEqual(viewer["results"][0]["status"],"REJECTED")
        code,_=self.request("GET","/api/v1/sync?cursor=bogus","test-b")
        self.assertEqual(code,400)


if __name__ == "__main__":
    unittest.main()
