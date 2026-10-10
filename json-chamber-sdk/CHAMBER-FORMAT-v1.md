# Chamber Format Specification — Version 1

**Status:** Draft  
**Date:** 2026-08-13  
**Product:** Chamber (Slid Phi Labs)  
**Scope:** Wire format for sealed JSON and arbitrary binary payloads  
**Out of scope:** License enforcement, trial periods, commercial unlocks, VerifiedDR gates

---

## 1. Abstract

Chamber Format v1 defines a **sealed object** that carries ciphertext plus two
keyword-encoded key shares. Either share alone is insufficient to recover the
message key. Opening requires both shares and a host master secret.

This document specifies only the **interchange format and cryptographic
construction**. Operational policy (time-bounded evaluation, kill-switch,
purchase unlock) is implementation-defined and is not part of this format.

---

## 2. Terminology

| Term | Meaning |
|------|---------|
| **Sealed object** | JSON object conforming to this specification |
| **Message key** | 32-byte AES-256 key used for one seal operation |
| **Share K / Share R** | Two 16-byte values produced by φ-split of the message key |
| **Master secret** | Host-held secret required at open time (implementation-supplied) |
| **AAD** | Additional Authenticated Data for AES-GCM: the ASCII bytes `chamber-v1` |

---

## 3. Sealed object (JSON)

A Chamber v1 sealed object is a JSON object with the following fields:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `v` | integer | yes | Format version. Must be `1`. |
| `algo` | string | yes | Algorithm identifier. Must be `"chamber-aes256gcm-phi"`. |
| `k_words` | string | yes | Space-separated word list encoding Share K (16 bytes). |
| `r_words` | string | yes | Space-separated word list encoding Share R (16 bytes). |
| `nonce` | string | yes | 12-byte AES-GCM nonce, lowercase hex (24 hex chars). |
| `tag` | string | yes | 16-byte public integrity tag, lowercase hex (32 hex chars). |
| `ct` | string | yes | AES-GCM ciphertext \|\| GCM tag, lowercase hex. |
| `orig_len` | integer | recommended | Plaintext length in bytes before sealing. |

### 3.1 Example

```json
{
  "v": 1,
  "algo": "chamber-aes256gcm-phi",
  "k_words": "ward veil spire lock oath cloak shard mark rune gate seal vault cipher glyph prism echo",
  "r_words": "aegis sigil forge anchor tide ember frost bloom thorn quill mirror hollow crown blade root star",
  "nonce": "a1b2c3d4e5f60718293a4b5c",
  "tag": "0123456789abcdef0123456789abcdef",
  "ct": "...",
  "orig_len": 42
}
```

(Word lists and hex values above are illustrative.)

### 3.2 Media type (informative)

Suggested media type for future registration:

```
application/chamber+json
```

Not yet registered with IANA.

---

## 4. Cryptographic construction

### 4.1 Message key

For each seal operation the implementation generates a fresh 32-byte message key
using a CSPRNG (`os.urandom(32)` or equivalent).

### 4.2 φ-split (2-of-2)

Let `key32` be the 32-byte message key.

1. `left  = key32[0:16]`
2. `right = key32[16:32]`
3. Interpret `left` as a big-endian 128-bit integer `bits`.
4. Rotate left by **21 bits**:
   ```
   rot = ((bits << 21) | (bits >> (128 - 21))) & ((1 << 128) - 1)
   ```
5. `K = rot` as 16 big-endian bytes.
6. `R = right XOR K` (byte-wise).

**Reassembly (φ-join):**

1. `right = R XOR K`
2. Interpret `K` as big-endian 128-bit integer `bits`.
3. Rotate right by 21 bits (inverse of the left rotate).
4. `left` = result as 16 bytes.
5. Message key = `left || right`.

Neither `K` nor `R` alone yields the message key.

**Rationale for 21:** Approximately `round(32 × (φ − 1))` where `φ ≈ 1.6180339887`,
chosen for a fixed, implementation-independent mix of the first half.

### 4.3 AES-256-GCM

| Parameter | Value |
|-----------|--------|
| Algorithm | AES-256-GCM (NIST SP 800-38D) |
| Key | 32-byte message key |
| Nonce | 12 random bytes per seal |
| AAD | ASCII `chamber-v1` (10 bytes) |
| Ciphertext field `ct` | `ciphertext ‖ GCM-tag` (GCM tag is 16 bytes) |

Decrypt must use the same AAD. Implementations MUST reject authentication
failures from GCM.

### 4.4 Public integrity tag

Separate from the GCM authentication tag:

```
tag = SHA-256(K ‖ R ‖ nonce)[0:16]
```

Stored as lowercase hex in the `tag` field.

On open, recompute from decoded shares and nonce; reject on mismatch
(constant-time compare recommended). This binds the two word lists to the nonce
before attempting GCM decrypt.

### 4.5 Master secret (open binding)

Opening requires a **master secret** supplied by the host environment.

Recommended derivation (informative; used by the reference SDK):

```
session = PBKDF2-HMAC-SHA256(
  password = master_secret_bytes,
  salt     = "chamber-phi-v1",
  iterations = 120000,
  dkLen    = 32
)
```

The format does **not** require that `session` be mixed into the message key.
The reference implementation uses the derivation as an environmental proof that
a master is present. Implementations MAY bind the master more tightly in future
format versions (`v > 1`).

---

## 5. Keyword encoding

Shares are transported as human-readable word lists, not raw binary.

### 5.1 Vocabulary

Exactly **64** words (6 bits per word), fixed order, indices `0 … 63`:

```
ward veil spire lock oath cloak shard mark
rune gate seal vault cipher glyph prism echo
aegis sigil forge anchor tide ember frost bloom
thorn quill mirror hollow crown blade root star
mist iron glass stone flame shade pulse nexus
orbit vector lattice helix core rim axis node
flux phase drift spark void bound link key
guard watch keep hold bind cast form rise
```

### 5.2 Bytes → words

1. Let `data` be the 16-byte share.
2. Pad with `0x00` so length is a multiple of 3.
3. For each 3-byte group `b0,b1,b2`:
   - `n = (b0 << 16) | (b1 << 8) | b2`
   - Emit four words: indices `(n >> 18) & 0x3F`, `(n >> 12) & 0x3F`,
     `(n >> 6) & 0x3F`, `n & 0x3F`.
4. Join with a single ASCII space.

### 5.3 Words → bytes

1. Split on whitespace; normalize to lowercase.
2. Map each word to its vocabulary index; unknown words are errors.
3. Consume tokens in groups of 4; rebuild 24-bit `n` and emit 3 bytes.
4. Truncate to the expected share length (16 bytes).

---

## 6. Seal procedure (normative outline)

1. Generate `msg_key` ← 32 random bytes.
2. `(K, R) ← φ-split(msg_key)`.
3. `nonce` ← 12 random bytes.
4. `ct ← AES-256-GCM-Encrypt(msg_key, nonce, plaintext, AAD="chamber-v1")`.
5. `tag ← SHA-256(K ‖ R ‖ nonce)[0:16]`.
6. Emit sealed object with `v=1`, `algo="chamber-aes256gcm-phi"`,
   word-encoded shares, hex `nonce` / `tag` / `ct`, and optional `orig_len`.

## 7. Open procedure (normative outline)

1. Reject if `v ≠ 1` or `algo ≠ "chamber-aes256gcm-phi"`.
2. Decode `k_words` → `K`, `r_words` → `R` (16 bytes each).
3. Decode `nonce`, recompute `tag`; reject on mismatch.
4. `msg_key ← φ-join(K, R)`.
5. Require host master secret (implementation policy).
6. `plaintext ← AES-256-GCM-Decrypt(msg_key, nonce, ct, AAD="chamber-v1")`.
7. Reject on GCM authentication failure.

---

## 8. Security properties (informative)

| Property | Status under this format |
|----------|---------------------------|
| Confidentiality of plaintext | AES-256-GCM with fresh key + nonce |
| Integrity of ciphertext | GCM tag inside `ct` |
| Binding of shares to nonce | Public `tag` = SHA-256(K‖R‖nonce)[:16] |
| Single-share uselessness | φ-split is 2-of-2; one share does not yield `msg_key` |
| Host binding | Master secret required at open (policy layer) |
| Forward secrecy across messages | Fresh `msg_key` per seal |

**Not provided by the format alone:** multi-party threshold beyond 2-of-2,
post-quantum resistance, or license lifetime.

---

## 9. Versioning

- This document defines **v = 1**.
- Unknown `v` values MUST be rejected.
- Future versions may change AAD, split, or master binding; they MUST use a
  new integer `v` and a distinct `algo` string.

---

## 10. Out of scope (explicit)

The following are **not** part of Chamber Format v1:

- Evaluation period duration (e.g. 24-hour trial)
- Kill-switch / `KILLED` marker behavior
- Purchase or unlock credentials
- Device fingerprinting
- MCP or other RPC tool gating
- Compression codecs (TRU8 and related products are separate)

Implementations MAY apply those policies around this format; interoperability
of sealed objects depends only on Sections 3–7.

---

## 11. Reference implementation

- Repository: https://github.com/ceedot-rock/json-chamber-sdk  
- Package: `json_chamber` (Python 3.10+)  
- Product page: https://www.slidphilabs.com/chamber  

The reference code is authoritative for edge cases not fully expanded here
(padding, word decoding errors, constant-time compares).

---

## 12. Change log

| Version | Date | Notes |
|---------|------|--------|
| 1 | 2026-08-13 | Initial public format draft aligned with json-chamber-sdk 0.1.0 |

---

*Chamber Format v1 — Slid Phi Labs*  
*Format only. Commercial licensing is separate.*
