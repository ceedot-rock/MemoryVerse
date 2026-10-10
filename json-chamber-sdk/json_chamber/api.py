"""
High-level and low-level Chamber APIs.
"""

from __future__ import annotations

import hashlib
import json
import os
from typing import Any

from . import core
from .license import require_cloak


def _master() -> bytes:
    raw = (
        os.environ.get("CHAMBER_MASTER_SECRET")
        or os.environ.get("JSON_CHAMBER_MASTER")
        or os.environ.get("TRU8_MASTER_SECRET", "chamber-demo-master-secret-32b!!")
    )
    b = raw.encode("utf-8")
    if len(b) < 32:
        b = hashlib.sha256(b).digest()
    return b


def cloak_bytes(data: bytes) -> dict:
    """Seal bytes. Requires a live cloak license (24h try, then month/year)."""
    require_cloak("json-chamber")
    return core.seal_aont(data, _master())


def open_bytes(sealed: dict) -> bytes:
    """Open a sealed blob. Keys only — no license, no clock, no kill switch."""
    return core.open_sealed(sealed, _master())


def cloak_json(obj: Any) -> dict:
    raw = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return cloak_bytes(raw)


def open_json(sealed: dict) -> Any:
    raw = open_bytes(sealed)
    return json.loads(raw.decode("utf-8"))
