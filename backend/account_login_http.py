"""ORUN S6A opt-in account HTTP gate on the existing authenticated SharedStore.

Only phone registrations approved in EdgeEnrollment + valid password can
create an account session. PC mirrors keep their pre-existing Ed25519
read-only bearer. Enrollment alone may inspect /sync/identity but cannot
read/write the shared resource ledger when account mode is enabled.
"""
from __future__ import annotations

import json
import sqlite3
from urllib.parse import urlsplit

from .account_login import AccountStore, LoginLocked, LoginRejected
from .edge_enrollment import EdgeEnrollment
from .phone_enrollment_http import mobile_handler_factory
from .shared_sync_store import SharedStore

MAX_LOGIN_BYTES = 1024


def account_handler_factory(store: SharedStore, enrollment: EdgeEnrollment,
                            accounts: AccountStore,
                            *, enable_phone_enrollment: bool = False):
    # No remote enrollment endpoints unless the operator separately opts in.
    if enable_phone_enrollment:
        Parent = mobile_handler_factory(store, enrollment)
    else:
        from .shared_sync_http import handler_factory
        Parent = handler_factory(store, enrollment.resolve_headers)

    class AccountHandler(Parent):
        @staticmethod
        def _bearer(headers: object) -> str | None:
            value = headers.get("Authorization", "") if hasattr(headers, "get") else ""
            if (not isinstance(value, str) or not value.startswith("Bearer ") or
                    value.count(" ") != 1):
                return None
            return value[7:]

        def _auth(self):
            bearer = self._bearer(self.headers)
            if bearer is None:
                return None
            account_actor = accounts.resolve_session(enrollment, bearer)
            if account_actor:
                return account_actor
            installation = enrollment.resolve_token_with_algorithm(bearer)
            if not installation:
                return None
            device, algorithm = installation
            # Existing PC mirror remains independent of Android account login.
            if (algorithm == "Ed25519" and
                    device.installation_id.startswith("pc-")):
                return device
            # Native Android enrollment must verify the server's site/ID
            # before the login password is entered, but this grants no
            # resource read/write access.
            if (algorithm == "P256-SHA256" and
                    urlsplit(self.path).path == "/api/v1/sync/identity"):
                return device
            return None

        def do_GET(self):  # noqa: N802
            url = urlsplit(self.path)
            if url.path != "/api/v1/account/me":
                return super().do_GET()
            if url.query or url.fragment:
                return self._send(404, {"error": "not_found"})
            bearer = self._bearer(self.headers)
            actor = accounts.resolve_session(enrollment, bearer) if bearer else None
            if not actor:
                return self._send(401, {"error": "unauthorized"})
            return self._send(200, {
                "site": actor.site_id, "actor": actor.actor_id,
                "installation": actor.installation_id,
                "scopes": sorted(actor.scopes),
            })

        def do_POST(self):  # noqa: N802
            url = urlsplit(self.path)
            if url.path not in (
                    "/api/v1/account/login", "/api/v1/account/logout"):
                return super().do_POST()
            if url.query or url.fragment:
                return self._send(404, {"error": "not_found"})
            bearer = self._bearer(self.headers)
            if not bearer:
                return self._send(401, {"error": "unauthorized"})
            if url.path == "/api/v1/account/logout":
                if self.headers.get("Content-Length") not in (None, "0"):
                    return self._send(400, {"error": "invalid_request"})
                if accounts.resolve_session(enrollment, bearer) is None:
                    return self._send(401, {"error": "unauthorized"})
                accounts.revoke_session(bearer)
                return self._send(200, {"signed_out": True})
            # Account login is available only with an APPROVED P256 device
            # bearer, never a bench token, PC mirror, anonymous caller or
            # account-supplied actor/site/permission claim.
            content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
            size = self.headers.get("Content-Length", "")
            if (self.headers.get("Transfer-Encoding") or
                    content_type != "application/json" or
                    not size.isascii() or not size.isdecimal() or
                    not 1 <= int(size) <= MAX_LOGIN_BYTES):
                return self._send(400, {"error": "invalid_request"})
            try:
                data = json.loads(self.rfile.read(int(size)).decode("utf-8"))
                if (not isinstance(data, dict) or
                        set(data) != {"username", "password"}):
                    return self._send(400, {"error": "invalid_request"})
                created = accounts.login(
                    enrollment, bearer, data["username"], data["password"])
                return self._send(200, {
                    "account_access_token": created.bearer,
                    "site": created.site, "actor": created.actor,
                    "username": created.username,
                    "installation": created.installation,
                    "expires_at": created.expires_at,
                })
            except LoginLocked:
                return self._send(429, {"error": "login_temporarily_restricted"})
            except (UnicodeDecodeError, json.JSONDecodeError):
                return self._send(400, {"error": "invalid_request"})
            except (LoginRejected, ValueError, TypeError):
                return self._send(401, {"error": "invalid_credentials"})
            except sqlite3.Error:
                return self._send(503, {"error": "temporarily_unavailable"})

    return AccountHandler
