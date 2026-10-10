"""json-chamber — JSON sealing protocol.

Cloak (seal new JSON) requires a live cloak license.
Open of an already-sealed blob is keys-only: no license, no clock.
TruGame and other engines still use require_alive() as a running-engine gate.
"""

from .api import cloak_json, open_json, cloak_bytes, open_bytes
from .core import WORDLIST
from .license import (
    LicenseError,
    PRODUCTS,
    apply_entitlement,
    create_entitlement,
    license_status,
    require_alive,
    require_cloak,
    reset_for_testing,
    verifieddr_check,
    verify_entitlement,
)
from .benefit import (
    benefit_check,
    tru8_benefit_check,
    group_then_pack,
    group_then_compress,
    delta_transform_positions,
    delta_transform_bytes,
    chunk_dedup,
    benefit_report,
)

__all__ = [
    # Sealing
    "cloak_json",
    "open_json",
    "cloak_bytes",
    "open_bytes",
    "WORDLIST",
    # Shared Black Box control plane
    "LicenseError",
    "PRODUCTS",
    "require_alive",
    "require_cloak",
    "license_status",
    "apply_entitlement",
    "create_entitlement",
    "verify_entitlement",
    "verifieddr_check",
    "reset_for_testing",
    # Optimizer / benefit
    "benefit_check",
    "tru8_benefit_check",
    "group_then_pack",
    "group_then_compress",
    "delta_transform_positions",
    "delta_transform_bytes",
    "chunk_dedup",
    "benefit_report",
]
__version__ = "1.4.1"
