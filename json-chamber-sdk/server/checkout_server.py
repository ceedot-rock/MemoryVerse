"""
Stripe Checkout + Entitlement Issuer for the shared Black Box control plane.

Mints signed entitlement tokens (json-chamber format) after payment.
Works for every product: json-chamber, tru8-chamber, chamber, trugame.

Run:
  pip install flask stripe
  export STRIPE_SECRET_KEY=sk_live_...
  export STRIPE_WEBHOOK_SECRET=whsec_...
  export STRIPE_PRICE_ID=price_...          # default json-chamber $99
  export DOMAIN=https://www.slidphilabs.com
  export JSON_CHAMBER_MASTER=...            # HMAC secret (must match client)
  python server/checkout_server.py
"""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import sys
import uuid
from pathlib import Path

from flask import Flask, jsonify, request

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from json_chamber.license import (  # noqa: E402
    PRODUCTS,
    create_entitlement,
    verify_entitlement,
)

app = Flask(__name__)

STRIPE_SECRET_KEY = os.getenv("STRIPE_SECRET_KEY", "")
STRIPE_WEBHOOK_SECRET = os.getenv("STRIPE_WEBHOOK_SECRET", "")
PRICE_ID = os.getenv("STRIPE_PRICE_ID", "")
DOMAIN = os.getenv("DOMAIN", "https://www.slidphilabs.com").rstrip("/")
MASTER_RAW = os.getenv("JSON_CHAMBER_MASTER") or os.getenv(
    "CHAMBER_MASTER_SECRET", "chamber-demo-master-secret-32b!!"
)
MASTER = MASTER_RAW.encode() if isinstance(MASTER_RAW, str) else MASTER_RAW
if len(MASTER) < 32:
    MASTER = hashlib.sha256(MASTER).digest()

DB_PATH = Path(os.getenv("CHAMBER_LICENSE_DB", "./licenses.json"))

try:
    import stripe

    stripe.api_key = STRIPE_SECRET_KEY
except ImportError:
    stripe = None  # type: ignore


def _load_db() -> dict:
    if not DB_PATH.exists():
        return {}
    return json.loads(DB_PATH.read_text(encoding="utf-8"))


def _save_db(db: dict) -> None:
    DB_PATH.write_text(json.dumps(db, indent=2), encoding="utf-8")


def mint_entitlement(
    *,
    product: str = "json-chamber",
    email: str = "",
    order_id: str = "",
    permanent: bool | None = None,
) -> dict:
    meta = PRODUCTS.get(product, PRODUCTS["json-chamber"])
    tier = meta["tier"]
    if permanent is None:
        permanent = tier == "one-time"
    token = create_entitlement(
        product=product,
        order_id=order_id or f"ord_{secrets.token_hex(8)}",
        email=email,
        box_id=str(uuid.uuid4()),
        tier=tier,
        permanent=permanent,
    )
    verify_entitlement(token)
    db = _load_db()
    key = token["box_id"]
    db[key] = {
        "token": token,
        "email": email,
        "order_id": order_id,
        "product": product,
        "tier": tier,
        "issued": token["issued"],
        "active": True,
        "price_usd": meta.get("price_usd", 0),
    }
    if order_id:
        db[f"order:{order_id}"] = key
    _save_db(db)
    return token


@app.route("/create-checkout-session", methods=["POST"])
def create_checkout_session():
    if stripe is None or not STRIPE_SECRET_KEY:
        return jsonify({"error": "stripe not configured"}), 500
    body = request.json or {}
    email = body.get("email", "")
    product = body.get("product", "json-chamber")
    if product not in PRODUCTS:
        product = "json-chamber"
    session = stripe.checkout.Session.create(
        mode="payment",
        line_items=[{"price": PRICE_ID, "quantity": 1}],
        success_url=(
            f"{DOMAIN}/access?product={product}"
            f"&session={{CHECKOUT_SESSION_ID}}"
        ),
        cancel_url=f"{DOMAIN}/access?canceled=1&product={product}",
        customer_email=email or None,
        metadata={
            "product": product,
            "price_usd": str(PRODUCTS[product].get("price_usd", 99)),
            "tier": PRODUCTS[product]["tier"],
        },
    )
    return jsonify({"url": session.url, "id": session.id, "product": product})


@app.route("/webhook", methods=["POST"])
def webhook():
    if stripe is None:
        return jsonify({"error": "stripe not configured"}), 500
    payload = request.data
    sig_header = request.headers.get("Stripe-Signature", "")
    try:
        if STRIPE_WEBHOOK_SECRET:
            event = stripe.Webhook.construct_event(
                payload, sig_header, STRIPE_WEBHOOK_SECRET
            )
        else:
            event = json.loads(payload)
    except Exception as e:
        return jsonify({"error": str(e)}), 400
    if event.get("type") == "checkout.session.completed":
        session = event["data"]["object"]
        email = (
            (session.get("customer_details") or {}).get("email")
            or session.get("customer_email")
            or ""
        )
        meta = session.get("metadata") or {}
        product = meta.get("product", "json-chamber")
        order_id = session.get("id", "")
        token = mint_entitlement(product=product, email=email, order_id=order_id)
        print(f"MINTED ENTITLEMENT for {email} product={product} box_id={token['box_id']}")
    return jsonify({"received": True})


@app.route("/entitlement/<box_id>")
def get_entitlement(box_id: str):
    db = _load_db()
    rec = db.get(box_id)
    if not rec or not rec.get("active"):
        return jsonify({"valid": False}), 404
    return jsonify({"valid": True, "entitlement": rec["token"]})


@app.route("/entitlement/by-order/<order_id>")
def get_entitlement_by_order(order_id: str):
    db = _load_db()
    box_id = db.get(f"order:{order_id}")
    if not box_id:
        return jsonify({"valid": False}), 404
    return get_entitlement(box_id)


@app.route("/mint", methods=["POST"])
def manual_mint():
    body = request.json or {}
    product = body.get("product", "json-chamber")
    email = body.get("email", "")
    order_id = body.get("order_id", f"manual_{secrets.token_hex(6)}")
    if product not in PRODUCTS:
        return jsonify({"error": f"unknown product {product}"}), 400
    token = mint_entitlement(product=product, email=email, order_id=order_id)
    return jsonify({"ok": True, "entitlement": token})


@app.route("/verify/<box_id>")
def verify_box(box_id: str):
    db = _load_db()
    rec = db.get(box_id)
    if not rec or not rec.get("active"):
        return jsonify({"valid": False})
    return jsonify(
        {
            "valid": True,
            "purchased": True,
            "email": rec.get("email"),
            "product": rec.get("product"),
            "tier": rec.get("tier"),
            "order_id": rec.get("order_id"),
        }
    )


@app.route("/")
def about():
    """Service descriptor: what this server is and every endpoint it offers."""
    return jsonify(
        {
            "service": "json-chamber-entitlement-server",
            "about": (
                "Stripe Checkout + entitlement issuer for the shared Black Box "
                "control plane (Chamber JSON sealing)."
            ),
            "version": "1.4.1",
            "endpoints": {
                "GET /": "this service descriptor",
                "GET /health": "liveness: ok flag, control plane, products, version",
                "POST /create-checkout-session": "start a Stripe Checkout session",
                "POST /webhook": "Stripe webhook; mints an entitlement on payment",
                "GET /entitlement/<box_id>": "read the entitlement for a box id",
                "GET /entitlement/by-order/<order_id>": "read the entitlement by order id",
                "POST /mint": "operator mint of an entitlement",
                "GET /verify/<box_id>": "verify a box entitlement",
            },
            "license": "AGPL-3.0-or-later OR Slid Phi Labs Commercial License",
            "contact": "corey@slidphilabs.com",
        }
    )


@app.route("/health")
def health():
    return jsonify(
        {
            "ok": True,
            "control_plane": "json-chamber",
            "products": list(PRODUCTS.keys()),
            "version": "1.2.0",
        }
    )


if __name__ == "__main__":
    print("Black Box entitlement server on :4242")
    print("Products:", ", ".join(PRODUCTS))
    print("Set STRIPE_SECRET_KEY, STRIPE_PRICE_ID, DOMAIN, JSON_CHAMBER_MASTER")
    app.run(port=4242, debug=False)
