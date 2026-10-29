"""Step 3C enrollment preflight — host tests only, genuine Ed25519."""
from __future__ import annotations

import hashlib
import http.client
import json
import os
from pathlib import Path
import sqlite3
import stat
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from backend.edge_enrollment import (
    EdgeEnrollment, EnrollmentError, EnrollmentRefused, signed_challenge
)
from backend.shared_sync_http import handler_factory
from backend.shared_sync_store import SharedStore

VIEWER = frozenset({"zone:read", "group:read"})
EDITOR = frozenset({"zone:read", "zone:write", "group:read", "group:write",
                    "zone_target:read", "zone_target:write"})
RING = [[376100000, 280500000], [376100000, 280510000],
        [376110000, 280510000], [376110000, 280500000]]


def keypair():
    key = Ed25519PrivateKey.generate()
    pub = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    return key, pub


def zone_op(uid="create"):
    return {"op_id": uid, "kind": "zone", "id": "z1", "action": "UPSERT",
            "base_revision": 0, "body": {"name": "Kuzey", "active": True,
                                         "ring_e7": RING}}


class EnrollmentTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "installations.db"
        self.enroll = EdgeEnrollment(self.path)

    def tearDown(self):
        self.tmp.cleanup()

    def prepare(self, install="phone-a", site="site-1", actor="owner", scopes=EDITOR):
        private, public = keypair()
        pending = self.enroll.begin(install, public)
        code = self.enroll.approve(pending.request_id, pending.fingerprint,
                                   site, actor, scopes)
        signature = private.sign(signed_challenge(pending, install, public))
        return private, public, pending, code, signature

    def test_admin_approval_requires_key_and_one_time_proof(self):
        _, public, pending, code, signature = self.prepare()
        self.assertIsNone(self.enroll.resolve_token("unauthorized"))
        token = self.enroll.finish(pending.request_id, "phone-a", public, signature, code)
        self.assertGreaterEqual(len(token), 48)
        actor = self.enroll.resolve_token(token)
        self.assertEqual(actor.site_id, "site-1")
        self.assertEqual(actor.installation_id, "phone-a")
        self.assertEqual(actor.scopes, EDITOR)
        self.assertIsNone(self.enroll.resolve_token(code))
        self.assertIsNone(self.enroll.resolve_token(token + "X"))
        self.assertEqual(EdgeEnrollment(self.path).resolve_token(token), actor)
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        with sqlite3.connect(self.path) as db:
            saved = db.execute("SELECT token_hash FROM edge_installs").fetchone()[0]
        self.assertEqual(saved, hashlib.sha256(token.encode()).digest())
        self.assertNotEqual(saved, token.encode())
        with self.assertRaises(EnrollmentRefused):
            self.enroll.finish(pending.request_id, "phone-a", public, signature, code)

    def test_unapproved_request_cannot_finish(self):
        private, public = keypair()
        pending = self.enroll.begin("phone-a", public)
        sig = private.sign(signed_challenge(pending, "phone-a", public))
        with self.assertRaises(EnrollmentRefused):
            self.enroll.finish(pending.request_id, "phone-a", public, sig, "x" * 43)

    def test_wrong_signature_and_fingerprint_do_not_issue(self):
        _, public, pending, code, signature = self.prepare()
        with self.assertRaises(EnrollmentRefused):
            self.enroll.finish(pending.request_id, "phone-a", public, b"\x01" * 64, code)
        with self.assertRaises(EnrollmentRefused):
            self.enroll.finish(pending.request_id, "phone-a", public, signature, code + "X")
        with self.assertRaises(EnrollmentRefused):
            self.enroll.finish(pending.request_id, "wrong-install", public, signature, code)
        token = self.enroll.finish(pending.request_id, "phone-a", public, signature, code)
        self.assertIsNotNone(self.enroll.resolve_token(token))
        with self.assertRaises(EnrollmentRefused):
            self.enroll.begin("phone-a", public)

    def test_unapproved_wrong_fingerprint_fails(self):
        _, public = keypair()
        pending = self.enroll.begin("phone-a", public)
        with self.assertRaises(EnrollmentRefused):
            self.enroll.approve(pending.request_id, "0" * 20, "site-a", "owner", EDITOR)

    def test_pending_expiry_and_expired_bearer(self):
        _, public, pending, code, signature = self.prepare()
        with sqlite3.connect(self.path) as db:
            db.execute("UPDATE edge_pending SET expires_at=0")
        with self.assertRaises(EnrollmentRefused):
            self.enroll.finish(pending.request_id, "phone-a", public, signature, code)
        # Fresh flow, old proof revoked by overwritten pending challenge.
        private, public = keypair()
        new = self.enroll.begin("phone-a", public)
        code = self.enroll.approve(new.request_id, new.fingerprint, "site-a", "owner", VIEWER)
        token = self.enroll.finish(new.request_id, "phone-a", public,
                                   private.sign(signed_challenge(new, "phone-a", public)), code)
        with sqlite3.connect(self.path) as db:
            db.execute("UPDATE edge_installs SET expires_at=0")
        self.assertIsNone(self.enroll.resolve_token(token))

    def test_revocation_and_independent_installations(self):
        first = self.prepare("phone-a", "site-a", "owner", EDITOR)
        second = self.prepare("phone-b", "site-a", "helper", VIEWER)
        ta = self.enroll.finish(first[2].request_id, "phone-a", first[1], first[4], first[3])
        tb = self.enroll.finish(second[2].request_id, "phone-b", second[1], second[4], second[3])
        self.assertIsNotNone(self.enroll.resolve_token(ta))
        self.assertIsNotNone(self.enroll.resolve_token(tb))
        self.assertTrue(self.enroll.revoke("phone-a"))
        self.assertFalse(self.enroll.revoke("phone-a"))
        self.assertIsNone(self.enroll.resolve_token(ta))
        self.assertIsNotNone(self.enroll.resolve_token(tb))

    def test_invalid_scope_and_permissive_malformed_actor_denied(self):
        _, pub = keypair()
        pending = self.enroll.begin("phone-a", pub)
        with self.assertRaises(EnrollmentError):
            self.enroll.approve(pending.request_id, pending.fingerprint, "site-a",
                                "owner", frozenset({"*"}))
        with self.assertRaises(EnrollmentError):
            self.enroll.approve(pending.request_id, pending.fingerprint, "site-a",
                                "owner", frozenset())
        with self.assertRaises(EnrollmentError):
            self.enroll.begin("../../etc/passwd", pub)

    def test_unapproved_request_rotates_but_cannot_cancel_owner_approved_request(self):
        private, pub = keypair()
        pending = self.enroll.begin("phone-a", pub)
        fresh = self.enroll.begin("phone-a", pub)
        self.assertNotEqual(pending.challenge, fresh.challenge)
        code = self.enroll.approve(fresh.request_id, fresh.fingerprint, "site-a", "owner", EDITOR)
        # An anonymous new begin must not erase an approval the owner already
        # compared. That would allow an attacker to cancel the enrollment.
        with self.assertRaises(EnrollmentRefused):
            self.enroll.begin("phone-a", pub)
        proof = private.sign(signed_challenge(fresh, "phone-a", pub))
        token = self.enroll.finish(fresh.request_id, "phone-a", pub, proof, code)
        self.assertIsNotNone(self.enroll.resolve_token(token))
        self.assertIsNone(self.enroll.resolve_token(code))


class EnrollmentHTTPTests(EnrollmentTests):
    def setUp(self):
        super().setUp()
        self.store=SharedStore(Path(self.tmp.name)/"shared.sqlite")
        self.srv=ThreadingHTTPServer(("127.0.0.1",0),
                handler_factory(self.store,self.enroll.resolve_headers))
        self.thread=threading.Thread(target=self.srv.serve_forever,daemon=True)
        self.thread.start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.thread.join(timeout=5)
        super().tearDown()

    def request(self,method,path,token=None,body=None,inject=None):
        conn=http.client.HTTPConnection("127.0.0.1",self.srv.server_port,timeout=5)
        headers={} if inject is None else dict(inject)
        if token:
            headers["Authorization"]="Bearer "+token
        conn.request(method,path,None if body is None else json.dumps(body),headers)
        res=conn.getresponse()
        code=res.status
        val=json.loads(res.read())
        conn.close()
        return code,val

    def test_real_principal_resolver_rejects_forged_claims_and_revocation(self):
        code,_=self.request("GET","/api/v1/sync",inject={
            "X-Site-ID":"site-a","X-Actor":"owner",
            "X-Scopes":"zone:read,zone:write"})
        self.assertEqual(code,401)
        a=self.prepare("phone-a","site-a","owner",EDITOR)
        ta=self.enroll.finish(a[2].request_id,"phone-a",a[1],a[4],a[3])
        b=self.prepare("phone-b","site-a","viewer",VIEWER)
        tb=self.enroll.finish(b[2].request_id,"phone-b",b[1],b[4],b[3])
        c=self.prepare("phone-c","site-b","owner",EDITOR)
        tc=self.enroll.finish(c[2].request_id,"phone-c",c[1],c[4],c[3])
        status,write=self.request("POST","/api/v1/sync/ops",ta,{"ops":[zone_op()]})
        self.assertEqual(status,200)
        self.assertEqual(write["results"][0]["status"],"ACCEPTED")
        status,read=self.request("GET","/api/v1/sync?cursor=0",tb)
        self.assertEqual(status,200)
        self.assertEqual(read["changes"][0]["body"]["name"],"Kuzey")
        status,other=self.request("GET","/api/v1/sync?cursor=0",tc)
        self.assertEqual(status,200)
        self.assertEqual(other["changes"],[])
        status,denied=self.request("POST","/api/v1/sync/ops",tb,{"ops":[zone_op("v1")]})
        self.assertEqual(status,200)
        self.assertEqual(denied["results"][0]["status"],"REJECTED")
        self.enroll.revoke("phone-a")
        self.assertEqual(self.request("GET","/api/v1/sync?cursor=0",ta)[0],401)
        self.assertEqual(self.request("GET","/api/v1/sync?cursor=0",tb)[0],200)


if __name__ == "__main__":
    unittest.main()
