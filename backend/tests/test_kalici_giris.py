"""1b: kalıcı giriş — e-posta ile giriş, 180 gün kayan oturum, çıkış."""
from __future__ import annotations

import hashlib
import sqlite3
import time
import unittest

from backend.account_login import PASSWORD_SESSION_SECONDS
from backend.tests import test_account_device_login as accounts_base

PASSWORD = accounts_base.PASSWORD


class KaliciGirisTests(unittest.TestCase):
    def setUp(self) -> None:
        base = accounts_base.AccountHTTPTests(methodName="test_owner_login_required_for_phone_shared_data")
        base.setUp()
        self.addCleanup(base.doCleanups)
        base.accounts.bootstrap_owner("orun-main", "owner", "ownerlogin", PASSWORD)
        self.base = base

    def login(self, identifier: str) -> tuple[int, dict]:
        return self.base.request("POST", "/api/v1/account/login",
                                 body={"username": identifier, "password": PASSWORD})

    def test_long_session_email_login_and_profile(self):
        status, body = self.login("ownerlogin")
        self.assertEqual(status, 200)
        token = body["account_access_token"]
        self.assertGreater(body["expires_at"], time.time() + PASSWORD_SESSION_SECONDS - 60)

        # E-posta ekle, sonra e-postayla (büyük/küçük harf fark etmez) gir.
        self.assertEqual(self.base.request("POST", "/api/v1/account/email", token,
                                           {"email": "Metay@Example.com"}),
                         (200, {"email": "metay@example.com"}))
        status, by_mail = self.login("METAY@example.com")
        self.assertEqual(status, 200)
        self.assertEqual(by_mail["username"], "ownerlogin")
        status, me = self.base.request("GET", "/api/v1/account/me", by_mail["account_access_token"])
        self.assertEqual((status, me["username"], me["email"]), (200, "ownerlogin", "metay@example.com"))
        self.assertIn("expires_at", me)

        # Girişsiz ve geçersiz e-posta reddedilir.
        self.assertEqual(self.base.request("POST", "/api/v1/account/email", None,
                                           {"email": "a@b.co"})[0], 401)
        self.assertEqual(self.base.request("POST", "/api/v1/account/email", token,
                                           {"email": "not-an-email"})[0], 400)
        self.assertEqual(self.login("wrong@example.com")[0], 401)

    def test_session_slides_forward_on_use_and_logout_ends_it(self):
        token = self.login("ownerlogin")[1]["account_access_token"]
        digest = hashlib.sha256(token.encode("ascii")).digest()
        soon = int(time.time()) + 3 * 24 * 3600
        with sqlite3.connect(self.base.accounts_path) as db:
            db.execute("UPDATE account_password_sessions SET expires_at=? WHERE token_hash=?",
                       (soon, digest))
        self.assertEqual(self.base.request("GET", "/api/v1/account/me", token)[0], 200)
        with sqlite3.connect(self.base.accounts_path) as db:
            renewed = db.execute("SELECT expires_at FROM account_password_sessions "
                                 "WHERE token_hash=?", (digest,)).fetchone()[0]
        self.assertGreater(renewed, time.time() + PASSWORD_SESSION_SECONDS - 60)

        self.assertEqual(self.base.request("POST", "/api/v1/account/logout", token)[0], 200)
        self.assertEqual(self.base.request("GET", "/api/v1/account/me", token)[0], 401)


if __name__ == "__main__":
    unittest.main()
