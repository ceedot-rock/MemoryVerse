"""
benefit.py — public-safe optimizer for the $99 → $1,900 funnel.

NO TRU8 residual engine here.
  • benefit_check / tru8_benefit_check  – entropy + zero-bit bias + pair entropy
  • group_then_pack / group_then_compress – create structure across tiny files
  • delta_transform_positions           – mesh/position structure before coding
  • chunk_dedup                         – cross-file repeat report (analysis only)

$99 path (json-chamber) always works via cloak_json / cloak_bytes.
$1,900 path is only advertised when compress=True.
"""

from __future__ import annotations

import hashlib
import io
import math
import struct
import tarfile
import zlib
from collections import Counter
from typing import Iterable, List, Mapping, Sequence, Tuple, Union


def _undress(data: bytes) -> List[int]:
    bits: List[int] = []
    for b in data:
        for i in range(7, -1, -1):
            bits.append((b >> i) & 1)
    return bits


def shannon_entropy(data: bytes) -> float:
    if not data:
        return 0.0
    n = len(data)
    counts = Counter(data)
    return -sum((v / n) * math.log2(v / n) for v in counts.values())


def _pair_entropy(bits: List[int]) -> float:
    if len(bits) < 2:
        return 0.0
    pairs = [(bits[i], bits[i + 1]) for i in range(0, len(bits) - 1)]
    n = len(pairs)
    c = Counter(pairs)
    return -sum((v / n) * math.log2(v / n) for v in c.values())


def benefit_check(data: bytes, *, min_bytes: int = 1024) -> dict:
    if data is None or len(data) == 0:
        return {
            "compress": False,
            "reason": "empty payload",
            "action": "json-chamber",
            "recommend": "json-chamber",
            "entropy": 0.0,
            "bias": 0.5,
            "pair_entropy": 0.0,
            "size": 0,
            "est_saving": 0.0,
            "price_hint": 99,
        }

    n = len(data)
    if n < min_bytes:
        return {
            "compress": False,
            "reason": f"tiny <{min_bytes}B - use json-chamber only ($99 tier); or group_then_pack first",
            "action": "json-chamber",
            "recommend": "group-first",
            "entropy": shannon_entropy(data),
            "bias": 0.5,
            "pair_entropy": 0.0,
            "size": n,
            "est_saving": 0.0,
            "price_hint": 99,
        }

    entropy = shannon_entropy(data)
    sample = data[:8192]
    bits = _undress(sample)
    zeros = bits.count(0)
    bias = (zeros / len(bits)) if bits else 0.5
    pair_entropy = _pair_entropy(bits)

    if entropy > 7.8:
        return {
            "compress": False,
            "reason": f"high entropy {entropy:.2f}/8.0 - already compressed/encrypted/random",
            "action": "json-chamber only + chunk dedup",
            "recommend": "json-chamber",
            "entropy": round(entropy, 4),
            "bias": round(bias, 4),
            "pair_entropy": round(pair_entropy, 4),
            "size": n,
            "est_saving": 0.0,
            "price_hint": 99,
        }

    est_saving = max(0.0, (8.0 - entropy) / 8.0)
    bias_boost = abs(bias - 0.5) * 0.4
    est_saving = min(0.95, est_saving + bias_boost)

    if est_saving < 0.08 and entropy > 7.0:
        return {
            "compress": False,
            "reason": f"low structure - entropy {entropy:.2f}, bias {bias:.3f}, est {est_saving*100:.1f}%",
            "action": "json-chamber",
            "recommend": "json-chamber",
            "entropy": round(entropy, 4),
            "bias": round(bias, 4),
            "pair_entropy": round(pair_entropy, 4),
            "size": n,
            "est_saving": round(est_saving, 4),
            "price_hint": 99,
        }

    return {
        "compress": True,
        "reason": (
            f"good candidate - entropy {entropy:.2f}, bias {bias:.3f}, "
            f"est {est_saving*100:.1f}% saving"
        ),
        "action": "tru8-chamber $1900 tier",
        "recommend": "tru8-chamber",
        "entropy": round(entropy, 4),
        "bias": round(bias, 4),
        "pair_entropy": round(pair_entropy, 4),
        "size": n,
        "est_saving": round(est_saving, 4),
        "price_hint": 1900,
        "note": "TRU8 engine is proprietary — contact license@slidphilabs.com",
    }


tru8_benefit_check = benefit_check


def delta_transform_positions(
    positions: Sequence[float],
    *,
    quantize: float = 1e-4,
) -> bytes:
    if not positions:
        return b""
    quantized = [int(round(p / quantize)) for p in positions]
    deltas = [quantized[0]]
    for i in range(1, len(quantized)):
        deltas.append(quantized[i] - quantized[i - 1])
    out = io.BytesIO()
    for d in deltas:
        zz = (d << 1) ^ (d >> 63) if d < 0 else (d << 1)
        while zz >= 0x80:
            out.write(bytes([(zz & 0x7F) | 0x80]))
            zz >>= 7
        out.write(bytes([zz & 0x7F]))
    return out.getvalue()


def delta_transform_bytes(raw: bytes, *, stride: int = 4, fmt: str = "<f") -> bytes:
    size = struct.calcsize(fmt)
    n = len(raw) // size
    if n == 0:
        return raw
    vals = list(struct.unpack("%d%s" % (n, fmt[-1] if fmt[-1] in "fd" else "f"), raw[: n * size]))
    return delta_transform_positions(vals)


def group_then_pack(
    files: Union[Mapping[str, bytes], Sequence[Tuple[str, bytes]]],
    *,
    mode: str = "tar",
) -> bytes:
    if isinstance(files, Mapping):
        items = list(files.items())
    else:
        items = list(files)
    if mode == "tar":
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tar:
            for name, payload in items:
                info = tarfile.TarInfo(name=str(name).replace("\\", "/").lstrip("/"))
                info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
        return buf.getvalue()
    buf = io.BytesIO()
    for name, data in items:
        name_b = str(name).encode("utf-8")
        buf.write(struct.pack("<I", len(name_b)))
        buf.write(name_b)
        buf.write(struct.pack("<I", len(data)))
        buf.write(data)
    return buf.getvalue()


def group_then_compress(
    files: Union[Mapping[str, bytes], Sequence[Tuple[str, bytes]]],
) -> dict:
    if isinstance(files, Mapping):
        items = list(files.items())
    else:
        items = list(files)
    total_raw = sum(len(d) for _, d in items)
    grouped_bytes = group_then_pack(items, mode="tar")
    check = benefit_check(grouped_bytes)
    compressed = zlib.compress(grouped_bytes, level=9)
    group_saving = 1.0 - (len(compressed) / len(grouped_bytes)) if grouped_bytes else 0.0
    individual_saving = []
    for _, data in items:
        if not data:
            individual_saving.append(0.0)
            continue
        c = zlib.compress(data, level=9)
        individual_saving.append(1.0 - (len(c) / len(data)))
    avg_individual = sum(individual_saving) / len(individual_saving) if individual_saving else 0.0
    return {
        "total_files": len(items),
        "total_raw": total_raw,
        "grouped_raw": len(grouped_bytes),
        "compressed": len(compressed),
        "group_saving": round(group_saving, 4),
        "avg_individual_saving": round(avg_individual, 4),
        "gain_from_grouping": round(group_saving - avg_individual, 4),
        "benefit_check": check,
        "recommendation": (
            f"Grouping created {(group_saving - avg_individual)*100:.1f}% extra saving"
            if group_saving > avg_individual
            else "Grouping did not beat per-file baseline"
        ),
        "note": "zlib baseline only — TRU8 residual engine is proprietary ($1,900 tier)",
    }


def chunk_dedup(files: Sequence[bytes], chunk_size: int = 4096) -> dict:
    seen = {}
    total_chunks = 0
    dup_chunks = 0
    for data in files:
        for i in range(0, len(data), chunk_size):
            chunk = data[i : i + chunk_size]
            h = hashlib.blake2b(chunk, digest_size=16).hexdigest()
            total_chunks += 1
            if h in seen:
                dup_chunks += 1
            else:
                seen[h] = True
    dedup_ratio = (dup_chunks / total_chunks) if total_chunks else 0.0
    return {
        "total_chunks": total_chunks,
        "unique_chunks": len(seen),
        "dup_chunks": dup_chunks,
        "dedup_saving": round(dedup_ratio, 4),
        "reason": (
            f"{dup_chunks}/{total_chunks} chunks duplicate - "
            f"{dedup_ratio*100:.1f}% saving via dedup even if entropy high"
        ),
        "action": "use chunk dedup + json-chamber for high entropy bundles",
        "note": "analysis only — production dedup packer is $1,900 tier",
    }


def benefit_report(*blobs: bytes, names: Sequence[str] | None = None) -> dict:
    per_file = []
    for i, b in enumerate(blobs):
        label = names[i] if names and i < len(names) else f"file_{i}"
        r = benefit_check(b)
        r["name"] = label
        per_file.append(r)
    grouped = None
    if len(blobs) >= 2:
        packed = group_then_pack({r["name"]: blobs[i] for i, r in enumerate(per_file)})
        grouped = benefit_check(packed)
        grouped["name"] = "__grouped__"
        grouped["packed_bytes"] = len(packed)
    any_compress = any(r.get("compress") for r in per_file) or (
        grouped.get("compress") if grouped else False
    )
    return {
        "files": per_file,
        "grouped": grouped,
        "any_compress_candidate": bool(any_compress),
        "funnel": (
            "tru8-chamber ($1,900/project/year)"
            if any_compress
            else "json-chamber ($99) — seal only"
        ),
    }
