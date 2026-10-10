from json_chamber import cloak_json, open_json

# 1. Cloak
payload = {"api_key": "sk-live-...", "webhook": "https://example.com/hook"}
sealed = cloak_json(payload)
print("SEALED:", sealed["k_words"][:60], "...")

# 2. k alone is unreadable
print("k_words alone:", sealed["k_words"].split()[:5], "-> random")

# 3. Open
opened = open_json(sealed)
print("OPENED:", opened)

# 4. Tamper test
try:
    sealed_bad = dict(sealed)
    sealed_bad["k_words"] = sealed_bad["k_words"].replace("chamber", "vault", 1)
    open_json(sealed_bad)
except Exception as e:
    print("TAMPER BLOCKED:", e)

# 5. Nonce uniqueness
sealed2 = cloak_json(payload)
print("Same payload, different words?", sealed["k_words"][:20] != sealed2["k_words"][:20])
