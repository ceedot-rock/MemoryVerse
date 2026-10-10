# Security Policy

## Supported versions

| Version | Supported |
| ------- | --------- |
| v0.1.x   | yes       |

## Reporting a vulnerability

Email Corey@slidphilabs.com with "PmCp security" in the subject.
Please include:

- what you found and where (file, function, op)
- a reproducer if you have one
- what you think the impact is (data corruption? crash? journal forgery?)

We aim to acknowledge within 48 hours and ship a fix or a mitigation plan
within 14 days. Please don't open a public issue for a live vulnerability —
give us the window first.

## Scope notes

- pmcp-core is a local C library: no network, no sandboxing, no privilege
  model. It trusts its caller the way libc trusts its caller.
- The threat surface that matters: journal integrity (a forged journal must
  not replay as legitimate history), torn-write atomicity, and extent
  isolation between regions.
- Chamber signing (milestone 2) is what turns journal entries into
  attributable provenance. Until then, `chamber_sig` is plumbed but zeroed.
