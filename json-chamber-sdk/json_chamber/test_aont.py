"""v1 still opens. v2 AONT needs both shares. One share is not enough."""
from json_chamber.core import seal, seal_aont, open_sealed

MASTER = b"chamber-demo-master-secret-32b!!"


def test_v1_roundtrip():
    s = seal(b"hello chamber", MASTER)
    assert s["v"] == 1
    assert open_sealed(s, MASTER) == b"hello chamber"


def test_v2_roundtrip_and_one_share_useless():
    s = seal_aont(b"hello aont", MASTER)
    assert s["v"] == 2
    assert open_sealed(s, MASTER) == b"hello aont"
    broken = dict(s)
    broken["k_words"] = s["r_words"]
    try:
        open_sealed(broken, MASTER)
        raise SystemExit("one-share must fail")
    except Exception:
        pass


if __name__ == "__main__":
    test_v1_roundtrip()
    test_v2_roundtrip_and_one_share_useless()
    print("ok")
