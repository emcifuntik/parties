#!/usr/bin/env python3
"""Reference implementation behind the golden vectors of parties-identity.

It shares no code with the Rust crate. It uses:

- `hashlib` and `hmac` of the Python standard library for SHA-256, SHA-512,
  HMAC and PBKDF2, and a hand-written HKDF (RFC 5869) on top of them;
- the `cryptography` package (OpenSSL) for Ed25519 and Argon2id. That is
  the same Ed25519 implementation the C++ client calls in
  `common/src/crypto.cpp`, so the version 1 vectors are what
  `derive_keypair` and `public_key_fingerprint` produce;
- the word list of `common/include/parties/bip39_wordlist.h`, read from the
  header itself.

The RFC 8032 and BIP-39 entries are the published vectors, typed in below;
the script only confirms them. Everything else is computed here from the
formulas in `docs/rust-rewrite/` (03 section 4; 04 sections 4, 5.2 and 6.3;
13 sections 5, 6.4 and 7.2).

Usage, from the repository root (Python 3.11+, cryptography 44+):

    python crates/parties-identity/tests/vectors/generate.py

It rewrites the JSON files next to itself. A change in any of them is a
change of the wire format or of a key derivation and needs a new version.
"""

import base64
import hashlib
import hmac
import json
import re
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.kdf.argon2 import Argon2id

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
WORDS = re.findall(
    r'"([a-z]+)"',
    (REPO / "common/include/parties/bip39_wordlist.h").read_text(encoding="utf-8"),
)
assert len(WORDS) == 2048 and WORDS == sorted(WORDS)
# SHA-256 of english.txt as published with BIP-39.
assert (
    hashlib.sha256(("\n".join(WORDS) + "\n").encode()).hexdigest()
    == "2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda"
)


# --- primitives -------------------------------------------------------------


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def hkdf_sha256(ikm: bytes, info: bytes, length: int) -> bytes:
    """RFC 5869 with no salt (a salt of 32 zero bytes)."""
    prk = hmac.new(b"\x00" * 32, ikm, hashlib.sha256).digest()
    okm, block, counter = b"", b"", 1
    while len(okm) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        okm += block
        counter += 1
    return okm[:length]


def signing_key(seed: bytes) -> Ed25519PrivateKey:
    return Ed25519PrivateKey.from_private_bytes(seed)


def public_key(seed: bytes) -> bytes:
    return signing_key(seed).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )


def sign(seed: bytes, message: bytes) -> bytes:
    return signing_key(seed).sign(message)


def fingerprint(public: bytes) -> str:
    return ":".join(f"{byte:02x}" for byte in sha256(public))


def six_words(public: bytes) -> str:
    bits = int.from_bytes(sha256(public), "big") >> (256 - 66)
    return " ".join(WORDS[(bits >> (11 * (5 - i))) & 0x7FF] for i in range(6))


def key_text(public: bytes) -> str:
    return base64.urlsafe_b64encode(public).decode().rstrip("=")


def mnemonic(entropy: bytes) -> str:
    checksum_bits = len(entropy) * 8 // 32
    bits = int.from_bytes(entropy, "big") << checksum_bits
    bits |= sha256(entropy)[0] >> (8 - checksum_bits)
    count = (len(entropy) * 8 + checksum_bits) // 11
    return " ".join(WORDS[(bits >> (11 * (count - 1 - i))) & 0x7FF] for i in range(count))


def bip39_seed(phrase: str, passphrase: str) -> bytes:
    salt = b"mnemonic" + passphrase.encode()
    return hashlib.pbkdf2_hmac("sha512", phrase.encode(), salt, 2048)


def v2_key_seed(phrase: str) -> bytes:
    return hkdf_sha256(bip39_seed(phrase, ""), b"parties/phrase/identity/v2", 32)


def level(public: bytes, counter: int) -> tuple[bytes, int]:
    digest = sha256(
        b"parties-identity-level-v1\x00" + public + counter.to_bytes(8, "little")
    )
    value = int.from_bytes(digest, "big")
    return digest, 256 - value.bit_length()


def seed_of(byte: int) -> bytes:
    return bytes([byte]) * 32


def write(name: str, about: str, body: dict) -> None:
    path = HERE / name
    text = json.dumps({"about": about, **body}, indent=2, ensure_ascii=True) + "\n"
    path.write_bytes(text.encode("ascii"))
    print(f"wrote {path.relative_to(REPO)}")


# --- RFC 8032, section 7.1 --------------------------------------------------

RFC8032 = [
    (
        "TEST 1",
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    ),
    (
        "TEST 2",
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    ),
    (
        "TEST 3",
        "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "af82",
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
    ),
    (
        "TEST SHA(abc)",
        "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
        "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704",
    ),
]


def rfc8032() -> None:
    cases = []
    for name, seed, public, message, signature in RFC8032:
        assert public_key(bytes.fromhex(seed)).hex() == public, name
        assert sign(bytes.fromhex(seed), bytes.fromhex(message)).hex() == signature, name
        cases.append(
            {
                "name": name,
                "seed": seed,
                "public_key": public,
                "message": message,
                "signature": signature,
            }
        )
    write(
        "rfc8032.json",
        "Ed25519 test vectors of RFC 8032 section 7.1, hex. 'seed' is the RFC's "
        "SECRET KEY. Typed in from the RFC and confirmed with OpenSSL by generate.py.",
        {"cases": cases},
    )


# --- BIP-39 -----------------------------------------------------------------

# From the vectors published with BIP-39 (trezor/python-mnemonic, vectors.json,
# English). The seed uses the passphrase "TREZOR".
BIP39 = [
    (
        "00000000000000000000000000000000",
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
        "c55257c360c07c72029aebc1b53c05ed0362ada38ead3e3e9efa3708e53495531f09a6987599d18264c1e1c92f2cf141630c7a3c4ab7c81b2f001698e7463b04",
    ),
    (
        "7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
        "legal winner thank year wave sausage worth useful legal winner thank yellow",
        "2e8905819b8723fe2c1d161860e5ee1830318dbf49a83bd451cfb8440c28bd6fa457fe1296106559a3c80937a1c1069be3a3a5bd381ee6260e8d9739fce1f607",
    ),
    (
        "80808080808080808080808080808080",
        "letter advice cage absurd amount doctor acoustic avoid letter advice cage above",
        "d71de856f81a8acc65e6fc851a38d4d7ec216fd0796d0a6827a3ad6ed5511a30fa280f12eb2e47ed2ac03b5c462a0358d18d69fe4f985ec81778c1b370b652a8",
    ),
    (
        "ffffffffffffffffffffffffffffffff",
        "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong",
        "ac27495480225222079d7be181583751e86f571027b0497b5b5d11218e0a8a13332572917f0f8e5a589620c6f15b11c61dee327651a14c34e18231052e48c069",
    ),
    (
        "000000000000000000000000000000000000000000000000",
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon agent",
        "035895f2f481b1b0f01fcf8c289c794660b289981a78f8106447707fdd9666ca06da5a9a565181599b79f53b844d8a71dd9f439c52a3d7b3e8a79c906ac845fa",
    ),
    (
        "0000000000000000000000000000000000000000000000000000000000000000",
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art",
        "bda85446c68413707090a52022edd26a1c9462295029f2e60cd7c4f2bbd3097170af7a4d73245cafa9c3cca8d561a7c3de6f5d4a10be8ed2a5e608d68f92fcc8",
    ),
    (
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo vote",
        "dd48c104698c30cfe2b6142103248622fb7bb0ff692eebb00089b32d22484e1613912f0a5b694407be899ffd31ed3992c456cdf60f5d4564b8ba3f05a69890ad",
    ),
    (
        "9e885d952ad362caeb4efe34a8e91bd2",
        "ozone drill grab fiber curtain grace pudding thank cruise elder eight picnic",
        "274ddc525802f7c828d8ef7ddbcdc5304e87ac3535913611fbbfa986d0c9e5476c91689f9c8a54fd55bd38606aa6a8595ad213d4c9c9f9aca3fb217069a41028",
    ),
]


def bip39() -> None:
    cases = []
    for entropy, phrase, seed in BIP39:
        assert mnemonic(bytes.fromhex(entropy)) == phrase
        assert bip39_seed(phrase, "TREZOR").hex() == seed
        cases.append({"entropy": entropy, "phrase": phrase, "seed": seed})
    write(
        "bip39.json",
        "Official BIP-39 test vectors (trezor/python-mnemonic vectors.json, English): "
        "entropy and seed in hex. The seeds use the passphrase 'TREZOR'; Parties "
        "itself derives identities with the empty passphrase (see identity.json). "
        "Typed in from the published file and confirmed by generate.py.",
        {"passphrase": "TREZOR", "cases": cases},
    )


# --- identities: keys, fingerprints, both derivations, passkeys --------------

ABOUT_PHRASE = (
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about"
)
ZOO_PHRASE = "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"
LETTER_PHRASE = (
    "letter advice cage absurd amount doctor acoustic avoid letter advice cage above"
)

# Phrases the C++ client accepts (validate_seed_phrase) and what derive_keypair
# makes of them. The first three are the phrases of tests/integration_test.cpp.
# The rest shows that the bytes are hashed as typed: white space changes the key.
V1_PHRASES = [
    ("integration test client A", ABOUT_PHRASE),
    ("integration test client B", ZOO_PHRASE),
    ("integration test client C", LETTER_PHRASE),
    ("no valid BIP-39 checksum", " ".join(["abandon"] * 12)),
    ("mixed words", "ozone drill grab fiber curtain grace pudding thank cruise elder eight zoo"),
    ("trailing space", ABOUT_PHRASE + " "),
    ("leading space", " " + ABOUT_PHRASE),
    ("two spaces after the first word", ABOUT_PHRASE.replace(" ", "  ", 1)),
    ("tab after the first word", ABOUT_PHRASE.replace(" ", "\t", 1)),
    ("trailing line feed", ABOUT_PHRASE + "\n"),
    ("trailing carriage return and line feed", ABOUT_PHRASE + "\r\n"),
    ("vertical tab and form feed", ABOUT_PHRASE.replace(" ", "\x0b", 1) + "\x0c"),
]

V2_ENTROPY = [
    "00000000000000000000000000000000",
    "7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
    "80808080808080808080808080808080",
    "ffffffffffffffffffffffffffffffff",
    "9e885d952ad362caeb4efe34a8e91bd2",
    "000102030405060708090a0b0c0d0e0f",
    "0000000000000000000000000000000000000000000000000000000000000000",
    "f585c11aec520db57dd353c69554b21a89b20fb0650966fa0a9d6f74fd989d8f",
]

PASSKEY_PRF = [
    "00" * 32,
    "42" * 32,
    "ff" * 32,
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
]

KEY_SEEDS = [
    "0707070707070707070707070707070707070707070707070707070707070707",
    "0000000000000000000000000000000000000000000000000000000000000000",
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
]


def identity() -> None:
    keys = []
    for seed in KEY_SEEDS:
        public = public_key(bytes.fromhex(seed))
        keys.append(
            {
                "seed": seed,
                "public_key": public.hex(),
                "public_key_text": key_text(public),
                "fingerprint": fingerprint(public),
                "six_words": six_words(public),
            }
        )

    v1 = []
    for name, phrase in V1_PHRASES:
        seed = sha256(phrase.encode())
        public = public_key(seed)
        v1.append(
            {
                "name": name,
                "phrase": phrase,
                "seed": seed.hex(),
                "public_key": public.hex(),
                "fingerprint": fingerprint(public),
            }
        )

    v2 = []
    for entropy in V2_ENTROPY:
        phrase = mnemonic(bytes.fromhex(entropy))
        seed = v2_key_seed(phrase)
        public = public_key(seed)
        v2.append(
            {
                "entropy": entropy,
                "phrase": phrase,
                "bip39_seed": bip39_seed(phrase, "").hex(),
                "seed": seed.hex(),
                "public_key": public.hex(),
                "fingerprint": fingerprint(public),
            }
        )

    passkey = []
    for prf in PASSKEY_PRF:
        entropy = hkdf_sha256(bytes.fromhex(prf), b"parties/passkey/identity/v1", 16)
        phrase = mnemonic(entropy)
        public = public_key(v2_key_seed(phrase))
        passkey.append(
            {
                "prf_output": prf,
                "entropy": entropy.hex(),
                "phrase": phrase,
                "public_key": public.hex(),
                "fingerprint": fingerprint(public),
            }
        )

    write(
        "identity.json",
        "Keys, fingerprints and key derivations of parties-identity, computed by "
        "generate.py with hashlib and OpenSSL, independently of the Rust code. Binary "
        "values are hex. 'keys': Ed25519 seed to public key, its text form (unpadded "
        "base64url), its fingerprint (colon-separated SHA-256, as the C++ "
        "public_key_fingerprint) and the six words of a server identity (first 66 "
        "bits of the fingerprint as BIP-39 words). 'v1': the C++ derive_keypair, "
        "seed = SHA-256(phrase bytes exactly as given). 'v2': BIP-39 phrase of the "
        "entropy, bip39_seed = PBKDF2-HMAC-SHA512(phrase, 'mnemonic', 2048, 64), "
        "seed = HKDF-SHA256(no salt, bip39_seed, 'parties/phrase/identity/v2', 32). "
        "'passkey': entropy = HKDF-SHA256(no salt, prf_output, "
        "'parties/passkey/identity/v1', 16), then as v2.",
        {"keys": keys, "v1": v1, "v2": v2, "passkey": passkey},
    )


# --- statements: handshake, password proof, rotation, key link, level --------


def argon2id(password: str, salt: bytes, memory_kib: int, iterations: int, lanes: int) -> bytes:
    return Argon2id(
        salt=salt, length=32, iterations=iterations, lanes=lanes, memory_cost=memory_kib
    ).derive(password.encode())


def statements() -> None:
    server_seed, user_seed, other_seed = seed_of(0x11), seed_of(0x22), seed_of(0x33)
    server, user, other = (public_key(seed) for seed in (server_seed, user_seed, other_seed))
    exporter = bytes(range(0xA0, 0xC0))

    server_proof_message = b"parties-server-proof-v2\x00" + exporter
    user_auth_message = b"parties-auth-v2\x00" + exporter + server + user
    handshake = {
        "exporter": exporter.hex(),
        "server_seed": server_seed.hex(),
        "server_identity": server.hex(),
        "user_seed": user_seed.hex(),
        "user_key": user.hex(),
        "server_proof_message": server_proof_message.hex(),
        "server_proof": sign(server_seed, server_proof_message).hex(),
        "user_auth_message": user_auth_message.hex(),
        "signature": sign(user_seed, user_auth_message).hex(),
    }

    passwords = []
    for password, salt, memory_kib, iterations, lanes in [
        ("correct horse battery staple", bytes(range(16)), 19456, 2, 1),
        ("", bytes(range(0x40, 0x60)), 19456, 2, 1),
        ("p\u00e4ssw\u00f6rd \u2603", bytes(range(0x10, 0x20)), 32768, 3, 4),
    ]:
        key = argon2id(password, salt, memory_kib, iterations, lanes)
        proof = hmac.new(key, b"parties-join-pw-v2\x00" + exporter, hashlib.sha256).digest()
        passwords.append(
            {
                "password": password,
                "salt": salt.hex(),
                "memory_kib": memory_kib,
                "iterations": iterations,
                "parallelism": lanes,
                "key": key.hex(),
                "exporter": exporter.hex(),
                "proof": proof.hex(),
            }
        )

    rotation_message = b"parties-server-rotation-v1\x00" + server + other
    rotation_signature = sign(server_seed, rotation_message)
    rotation = {
        "old_seed": server_seed.hex(),
        "old_key": server.hex(),
        "new_key": other.hex(),
        "message": rotation_message.hex(),
        "signature": rotation_signature.hex(),
        "statement": (server + other + rotation_signature).hex(),
    }

    link_message = b"parties-link-key-v1\x00" + exporter + server + user + other
    key_link = {
        "exporter": exporter.hex(),
        "server_identity": server.hex(),
        "account_seed": user_seed.hex(),
        "account_key": user.hex(),
        "new_seed": other_seed.hex(),
        "new_key": other.hex(),
        "message": link_message.hex(),
        "proof": sign(other_seed, link_message).hex(),
        "approval": sign(user_seed, link_message).hex(),
    }

    levels = []
    for seed in (seed_of(0x07), server_seed, user_seed):
        public = public_key(seed)
        for counter in (0, 1, 2, 255, 256, 2**32, 2**64 - 1):
            digest, reached = level(public, counter)
            levels.append(
                {
                    "public_key": public.hex(),
                    "counter": counter,
                    "digest": digest.hex(),
                    "level": reached,
                }
            )
    searches = []
    for seed in (seed_of(0x07), server_seed):
        public = public_key(seed)
        for target in (1, 4, 8, 12, 16):
            counter = 0
            while level(public, counter)[1] < target:
                counter += 1
            searches.append(
                {
                    "public_key": public.hex(),
                    "target": target,
                    "first_counter": counter,
                    "level": level(public, counter)[1],
                }
            )

    write(
        "statements.json",
        "Signed and hashed statements of parties-identity, computed by generate.py "
        "with hashlib and OpenSSL, independently of the Rust code. Binary values are "
        "hex; '||' is concatenation. 'handshake' (document 03 section 4): "
        "server_proof = Ed25519(server key, 'parties-server-proof-v2' || 00 || "
        "exporter); signature = Ed25519(user key, 'parties-auth-v2' || 00 || exporter "
        "|| server_identity || user_key). 'password': key = Argon2id v1.3 (password "
        "UTF-8, salt, memory, passes, lanes, 32 bytes); proof = HMAC-SHA256(key, "
        "'parties-join-pw-v2' || 00 || exporter). 'rotation': signature = "
        "Ed25519(old key, 'parties-server-rotation-v1' || 00 || old_key || new_key); "
        "statement = old_key || new_key || signature. 'key_link': message = "
        "'parties-link-key-v1' || 00 || exporter || server_identity || account_key || "
        "new_key; proof = Ed25519(new key, message); approval = Ed25519(account key, "
        "message). 'level': digest = SHA-256('parties-identity-level-v1' || 00 || "
        "public_key || counter as 8 bytes little-endian); level = leading zero bits. "
        "'level_search': the smallest counter from 0 that reaches the target.",
        {
            "handshake": handshake,
            "password": passwords,
            "rotation": rotation,
            "key_link": key_link,
            "level": levels,
            "level_search": searches,
        },
    )


if __name__ == "__main__":
    rfc8032()
    bip39()
    identity()
    statements()
