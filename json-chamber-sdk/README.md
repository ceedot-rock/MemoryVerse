# json-chamber

[![Audited checks](https://github.com/ceedot-rock/json-chamber-sdk/actions/workflows/audited-checks.yml/badge.svg)](https://github.com/ceedot-rock/json-chamber-sdk/actions/workflows/audited-checks.yml)
[![License](https://img.shields.io/badge/license-AGPL--3.0--or--later%20%7C%20Commercial-blue.svg)](LICENSE)

**Chamber** is a two-key JSON sealing protocol from **Slid Phi Labs**, built for
agents and services that must store secrets they can't afford to leak — API
keys, credentials, and payloads at rest. One key seals, two independent keys
open; the ciphertext never expires, and opening an already-sealed blob needs
nothing but the keys — no license, no clock. Sealing new JSON is a cloak
license: free 24-hour try, then **$9/month** or **$99/year**.

Pure security product from **Slid Phi Labs**. No compressor engine in this package.

## What you buy

A **cloak license** — the right to seal *new* JSON for a term.

| SKU | What it is | Price |
|-----|------------|-------|
| 24-hour try | cloak new JSON | Free |
| Chamber month | cloak license | **$9 / month** |
| Chamber year | cloak license | **$99 / year** |

**Open** of an already-sealed blob is both keys, no extra payment, no clock. If the license lapses you cannot seal new JSON; you can still open what you already sealed.

Live checkout: https://www.slidphilabs.com/chamber

## Install

```bash
pip install json-chamber
```

Editable (lab): `pip install -e .`

## Quick start

```python
from json_chamber import cloak_json, open_json

sealed = cloak_json({"api_key": "sk-live-...", "webhook": "https://example.com/hook"})
original = open_json(sealed)  # keys only — works even after the cloak license ends
```

## License gate

- `cloak_json` / `cloak_bytes` → `require_cloak()` (24h try, then month/year)
- `open_json` / `open_bytes` → keys only. Never killed by the license clock.

## Format

**[Chamber Format Spec v1](./CHAMBER-FORMAT-v1.md)** — wire format only.

## License

Dual-licensed: **AGPL-3.0-or-later OR Slid Phi Labs Commercial License** — see [LICENSE](./LICENSE).
**No compressor engine in this package.**

## Links

- Product: https://www.slidphilabs.com/chamber
- MCP: https://github.com/ceedot-rock/json-chamber-mcp
- Contact: corey@slidphilabs.com

## From the same lab

- **AwLPay** — multi-rail agent payments (USDC x402 on Base and Solana, PayPal sandbox bridge): https://github.com/ceedot-rock/awlpay
- **agenTill** — drop-in payment box that turns any online product into a storefront agents can buy from: https://github.com/ceedot-rock/agenTill
- **ExactOdds** — provably-fair game math, byte-identical rules across five languages: https://github.com/ceedot-rock/exactodds
- **TNSSRC** — local lossless compression engine (Silesia 43,724,575 bytes, 12/12 decode+SHA verified): https://github.com/ceedot-rock/neural-pcc
- **pulsar** — free local best-path compressor (GPLv3 demo, not PCC): https://github.com/ceedot-rock/pulsar-best
- **TRUSTREAM** — lossless compression for live data streams in 4 KiB tiles: https://github.com/ceedot-rock/trustream
- Lab site: https://www.slidphilabs.com
