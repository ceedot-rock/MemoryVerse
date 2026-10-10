# Security Policy

Chamber is a secrecy protocol. A bug that weakens the seal, leaks a secret,
bypasses the cloak license gate, or lets a tampered entitlement pass
verification is a security issue, not a normal bug.

## Reporting a vulnerability

Please do not open a public issue for security problems.

- Email: corey@slidphilabs.com with the subject line `json-chamber security`
- Or use GitHub's private vulnerability reporting on this repository
  (Security tab, "Report a vulnerability")

Include the affected file, steps or inputs to reproduce, and what you
expected versus what happened.

You can expect an acknowledgement within 3 business days. We will keep you
updated while we investigate and credit you in the changelog unless you
prefer to stay anonymous.

## In scope

- Seal weaknesses: any input where the sealed blob leaks plaintext or a
  reduced key search
- AONT failures: a v2 blob that opens with one share or a corrupted share
- Entitlement forgery: a tampered token that passes `verify_entitlement`
- License-gate bypass: sealing new JSON without a live cloak license
- The `json-chamber` PyPI package and `server/checkout_server.py`

## Out of scope

- Operator deployments we do not run (bring your own Stripe keys securely)
- Social engineering, spam, or denial-of-service against hosted checkouts
