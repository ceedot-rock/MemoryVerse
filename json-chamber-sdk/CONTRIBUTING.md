# Contributing to json-chamber-sdk

Thanks for helping make Chamber sealing solid.

## Ground rules

- The wire format is frozen in [CHAMBER-FORMAT-v1.md](./CHAMBER-FORMAT-v1.md).
  Do not change it without updating the spec in the same PR.
- v1 sealed blobs must always open. v2 AONT needs both keyword shares.
- Seal/open round-trips must be exact; tests must prove it.
- No real secrets, keys, or entitlements in code, fixtures, or commits.

## Quick checks

```sh
pip install -e ".[dev]"
python -m pytest -q
```

CI runs the test suite and a checkout-server smoke test
(boots `server/checkout_server.py`, hits `/health` and `/`) on every
pull request.

## Working on the entitlement server

```sh
pip install -e ".[server]"
python server/checkout_server.py   # :4242
```

The server is Stripe-backed. `curl localhost:4242/health` must answer
`{"ok": true, ...}`. `curl localhost:4242/` describes every endpoint.

## Licensing

json-chamber-sdk is dual-licensed (AGPL-3.0-or-later OR the Slid Phi Labs
Commercial License). By contributing you agree your contribution may be
distributed under both.
