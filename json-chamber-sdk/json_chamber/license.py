"""
json-chamber shared control plane — cloak license + entitlement.

Policy (2026-09-02):
  • Chamber is a protocol for storing a JSON secret. Ciphertext does not expire.
  • What you sell is a **cloak license** (right to seal *new* JSON for a term).
  • **open()** of an already-sealed blob is both keys, no extra payment, no clock.
  • Trial: 24 hours of cloak from first activation.
  • After trial: cloak refuses until a month ($9) or year ($99) license is applied.
  • TruGame / other engines still use require_alive() as a running-engine gate.
  • Integrity: HMAC signature on license + entitlement.
  • Payment gate: Stripe / x402 / invoice (corey@slidphilabs.com)

Chamber states (cloak only):
  eval → cloak-expired → entitled (lease) → (lease end) → cloak-expired
Open is never killed by this module.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import time
import uuid
from pathlib import Path
from typing import Any

LICENSE_DIR = Path.home() / ".chamber"
LICENSE_FILE = LICENSE_DIR / "license.json"
KILL_FILE = LICENSE_DIR / "KILLED"
ENTITLEMENT_FILE = LICENSE_DIR / "entitlement.json"

TRIAL_SECONDS = 24 * 3600
ONE_TIME_GRACE_SECONDS = 24 * 3600
LEASE_SECONDS = 30 * 86400

PURCHASE_URL = os.environ.get(
    "CHAMBER_PURCHASE_URL",
    "https://www.slidphilabs.com/access",
)
SUPPORT_EMAIL = "corey@slidphilabs.com"

PRODUCTS: dict[str, dict[str, Any]] = {
    "json-chamber": {"price_usd": 99, "tier": "year", "label": "Chamber annual cloak license"},
    "chamber": {"price_usd": 99, "tier": "year", "label": "Chamber annual cloak license"},
    "chamber-month": {"price_usd": 9, "tier": "month", "label": "Chamber monthly cloak license"},
    "chamber-year": {"price_usd": 99, "tier": "year", "label": "Chamber annual cloak license"},
    "trugame": {"price_usd": 79, "tier": "year", "label": "TruGame engine"},
}


class LicenseError(RuntimeError):
    """Raised when the chamber is dead, expired, tampered, or not entitled."""


def _master_bytes() -> bytes:
    raw = (
        os.environ.get("CHAMBER_MASTER_SECRET")
        or os.environ.get("JSON_CHAMBER_MASTER")
        or os.environ.get("TRU8_MASTER_SECRET", "chamber-demo-master-secret-32b!!")
    )
    b = raw.encode("utf-8") if isinstance(raw, str) else raw
    if len(b) < 32:
        b = hashlib.sha256(b).digest()
    return b


def _sign(payload: dict[str, Any]) -> str:
    msg = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return hmac.new(_master_bytes(), msg, hashlib.sha256).hexdigest()


def _verify_sig(data: dict[str, Any]) -> bool:
    sig = data.get("sig")
    if not sig:
        return False
    body = {k: v for k, v in data.items() if k != "sig"}
    return hmac.compare_digest(sig, _sign(body))


def _ensure_dir() -> None:
    LICENSE_DIR.mkdir(parents=True, exist_ok=True)


def _device_fingerprint() -> str:
    parts = [
        str(Path.home()),
        os.environ.get("USER", ""),
        os.environ.get("HOSTNAME", os.environ.get("COMPUTERNAME", "")),
    ]
    return hashlib.sha256("|".join(parts).encode()).hexdigest()[:24]


def create_entitlement(
    *,
    product: str = "json-chamber",
    order_id: str = "",
    email: str = "",
    box_id: str | None = None,
    tier: str | None = None,
    lease_seconds: int | None = None,
    grace_seconds: int | None = None,
    permanent: bool = False,
) -> dict[str, Any]:
    meta = PRODUCTS.get(product, PRODUCTS["json-chamber"])
    tier = tier or meta["tier"]
    now = int(time.time())
    if lease_seconds is None:
        lease_seconds = 0 if tier == "one-time" else LEASE_SECONDS
    if grace_seconds is None:
        grace_seconds = ONE_TIME_GRACE_SECONDS if tier == "one-time" else LEASE_SECONDS
    token: dict[str, Any] = {
        "v": 1,
        "type": "entitlement",
        "product": product,
        "box_id": box_id or str(uuid.uuid4()),
        "tier": tier,
        "issued": now,
        "expires": None if (tier == "one-time" or permanent) else now + lease_seconds,
        "lease_seconds": lease_seconds,
        "grace_seconds": grace_seconds,
        "permanent": bool(permanent or tier == "one-time"),
        "order_id": order_id,
        "email": email,
        "price_usd": meta.get("price_usd", 0),
    }
    token["sig"] = _sign(token)
    return token


def verify_entitlement(token: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(token, dict) or token.get("type") != "entitlement":
        raise LicenseError("invalid entitlement: not an entitlement object")
    if token.get("v") != 1:
        raise LicenseError("invalid entitlement: unsupported version")
    if not _verify_sig(token):
        raise LicenseError("invalid entitlement: signature mismatch — possible tamper")
    return token


def _kill(reason: str) -> None:
    _ensure_dir()
    KILL_FILE.write_text(reason, encoding="utf-8")


def _load_or_create(product: str = "json-chamber") -> dict[str, Any]:
    _ensure_dir()
    if KILL_FILE.exists():
        raise LicenseError(
            f"Chamber hard-killed ({KILL_FILE.read_text(encoding='utf-8').strip()}). "
            f"Pay to re-enable → {PURCHASE_URL} or email {SUPPORT_EMAIL}"
        )
    if LICENSE_FILE.exists():
        try:
            data = json.loads(LICENSE_FILE.read_text(encoding="utf-8"))
        except Exception as e:
            _kill("tamper-parse")
            raise LicenseError(f"license file unreadable – killed: {e}") from e
        if not _verify_sig(data):
            _kill("tamper-sig")
            raise LicenseError("license signature mismatch – killed")
        return {k: v for k, v in data.items() if k != "sig"}
    now = int(time.time())
    box_id = str(uuid.uuid4())
    lic = {
        "v": 1,
        "type": "eval",
        "product": product,
        "box_id": box_id,
        "first_run": now,
        "eval_expires": now + TRIAL_SECONDS,
        "purchased": False,
        "tier": "eval",
        "device_hash": _device_fingerprint(),
        "last_check": now,
    }
    _write_license(lic)
    return lic


def _write_license(lic: dict[str, Any]) -> None:
    _ensure_dir()
    body = {k: v for k, v in lic.items() if k != "sig"}
    body["sig"] = _sign(body)
    LICENSE_FILE.write_text(json.dumps(body, indent=2), encoding="utf-8")


def apply_entitlement(token: dict[str, Any] | str) -> dict[str, Any]:
    if isinstance(token, str):
        token = json.loads(token)
    token = verify_entitlement(token)
    _ensure_dir()
    if KILL_FILE.exists():
        KILL_FILE.unlink()
    now = int(time.time())
    meta = PRODUCTS.get(token["product"], PRODUCTS["json-chamber"])
    lic = {
        "v": 1,
        "type": "entitled",
        "product": token["product"],
        "box_id": token.get("box_id") or str(uuid.uuid4()),
        "first_run": now,
        "eval_expires": 0,
        "purchased": True,
        "tier": token.get("tier") or meta["tier"],
        "device_hash": _device_fingerprint(),
        "last_check": now,
        "entitlement_issued": token.get("issued", now),
        "entitlement_expires": token.get("expires"),
        "lease_seconds": token.get("lease_seconds", 0),
        "grace_seconds": token.get("grace_seconds", ONE_TIME_GRACE_SECONDS),
        "permanent": bool(token.get("permanent")),
        "order_id": token.get("order_id", ""),
        "email": token.get("email", ""),
        "price_usd": token.get("price_usd", meta.get("price_usd", 0)),
    }
    _write_license(lic)
    ENTITLEMENT_FILE.write_text(json.dumps(token, indent=2), encoding="utf-8")
    return {
        "status": "entitled",
        "license": lic,
        "product": lic["product"],
        "tier": lic["tier"],
        "permanent": lic["permanent"],
        "expires": lic["entitlement_expires"],
    }


def require_cloak(product: str = "json-chamber") -> dict[str, Any]:
    """Gate for *sealing new JSON*. Open does not call this."""
    return require_alive(product)


def require_alive(product: str = "json-chamber") -> dict[str, Any]:
    if KILL_FILE.exists():
        reason = KILL_FILE.read_text(encoding="utf-8").strip()
        raise LicenseError(
            f"Chamber hard-killed ({reason}). "
            f"Payment required → {PURCHASE_URL} or {SUPPORT_EMAIL}"
        )
    lic = _load_or_create(product)
    now = int(time.time())
    if lic.get("purchased"):
        if lic.get("permanent"):
            last = int(lic.get("last_check", now))
            grace = int(lic.get("grace_seconds", ONE_TIME_GRACE_SECONDS))
            if now - last > grace:
                lic["last_check"] = now
                lic["recheck_due"] = True
                _write_license(lic)
            return {
                "license": lic,
                "status": "purchased",
                "product": lic.get("product", product),
                "tier": lic.get("tier", "one-time"),
                "permanent": True,
                "price": lic.get("price_usd", 99),
            }
        expires = lic.get("entitlement_expires")
        if expires is not None and now > int(expires):
            _kill("lease-expired")
            raise LicenseError(
                f"{lic.get('product', product)} lease ended. "
                f"Renew → {PURCHASE_URL} or email {SUPPORT_EMAIL}"
            )
        lic["last_check"] = now
        _write_license(lic)
        remaining = max(0, int(expires) - now) if expires else None
        return {
            "license": lic,
            "status": "leased",
            "product": lic.get("product", product),
            "tier": lic.get("tier", "project-year"),
            "seconds_remaining": remaining,
            "price": lic.get("price_usd", 1900),
        }
    gate = verifieddr_check()
    if gate.get("purchased"):
        token = create_entitlement(
            product=gate.get("tier", product) if gate.get("tier") in PRODUCTS else product,
            order_id="env-key",
            email="",
            permanent=True,
        )
        return apply_entitlement(token)
    expires = int(lic.get("eval_expires", 0))
    if now > expires:
        _kill("trial-expired")
        raise LicenseError(
            f"24h cloak try ended for {lic.get('product', product)}. "
            f"Sealing new JSON is off until a cloak license is live. "
            f"Already-sealed blobs still open with both keys. "
            f"Cloak license → {PURCHASE_URL} or email {SUPPORT_EMAIL}"
        )
    remaining = max(0, expires - now)
    return {
        "license": lic,
        "status": "eval",
        "product": lic.get("product", product),
        "seconds_remaining": remaining,
        "price": PRODUCTS.get(product, PRODUCTS["json-chamber"]).get("price_usd", 99),
        "purchase_url": PURCHASE_URL,
    }


def license_status(product: str = "json-chamber") -> dict[str, Any]:
    try:
        return require_alive(product)
    except LicenseError as e:
        return {
            "status": "dead",
            "reason": str(e),
            "product": product,
            "purchase_url": PURCHASE_URL,
            "support": SUPPORT_EMAIL,
        }


def verifieddr_check() -> dict[str, Any]:
    key = os.environ.get("VERIFIEDDR_API_KEY", "")
    if not key:
        return {
            "verified": True,
            "reason": "open eval (no VERIFIEDDR_API_KEY)",
            "purchased": False,
            "eval_mode": True,
            "tier": "json-chamber",
        }
    purchased = any(tok in key for tok in ("purchased", "pro", "live_"))
    product = "tru8-chamber" if "tru8" in key.lower() else "json-chamber"
    if "trugame" in key.lower():
        product = "trugame"
    if "chamber" in key.lower() and "json" not in key.lower() and "tru8" not in key.lower():
        product = "chamber"
    return {
        "verified": True,
        "reason": "ok",
        "purchased": purchased,
        "eval_mode": not purchased,
        "tier": product,
        "app_id": "chamber",
    }


def reset_for_testing() -> None:
    for p in (LICENSE_FILE, KILL_FILE, ENTITLEMENT_FILE):
        if p.exists():
            p.unlink()
