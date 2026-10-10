# Chamber cloak-license policy (json-chamber shared)

**Status:** 2026-09-02  
**Control plane:** `json_chamber.license`  
**Applies to:** json-chamber / chamber cloak. TruGame uses `require_alive()` as a running-engine gate.

## Split

Chamber is a **protocol for storing a JSON secret**.

| Call | Gate |
|------|------|
| `cloak_*` (seal new JSON) | live cloak license (`require_cloak`) |
| `open_*` (open already-sealed blob) | **keys only** — no license, no clock, no kill switch |

Ciphertext does not expire. If the cloak license lapses, you cannot seal new JSON. You can still open what you already sealed.

## Cloak lifecycle

1. **First activation** → 24-hour cloak try (`first_run` + `eval_expires`).
2. **After 24 hours** → cloak refuses until payment.
3. **Payment** (Stripe, x402, or invoice `corey@slidphilabs.com`) → signed entitlement token.
4. **Client applies entitlement** → cloak on for the purchased period ($9/mo or $99/yr).
5. **Lease end** → cloak off until renew. Open still works.

## Client gate

```python
from json_chamber import require_cloak, LicenseError, cloak_json, open_json

try:
    sealed = cloak_json({"secret": "…"})   # licensed
except LicenseError:
    # cloak off — buy month/year
    ...

original = open_json(sealed)  # keys only
```

TruGame / other engines:

```python
from json_chamber import require_alive, LicenseError
require_alive("trugame")
```

## Server

`server/checkout_server.py` mints entitlement tokens on Stripe `checkout.session.completed`.

## Integrity

- Local `license.json` is HMAC-signed; tamper kills **cloak**, not open.
- Entitlement tokens are HMAC-signed with the same master secret.

## Contact

Support / PACKAGE ACCESS: **corey@slidphilabs.com**
