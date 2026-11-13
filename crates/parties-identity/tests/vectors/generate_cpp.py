#!/usr/bin/env python3
"""Writes cpp_v1.json: what the C++ client itself makes of recovery phrases
and of private keys in hex.

Unlike generate.py, nothing here is computed in Python. The inputs are fed
to cpp_reference (built from cpp_reference.cpp and common/src/crypto.cpp;
the build command is in that file) and its answers are recorded. The Rust
tests then demand the same answers from parties-identity: which texts are a
phrase at all, and which key and fingerprint each one gives.

Usage, from the repository root:

    python crates/parties-identity/tests/vectors/generate_cpp.py <path to cpp_reference>

The inputs are fixed (hand-written cases plus a seeded random sample), so a
second run writes the same file.
"""

import json
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
WORDS = re.findall(
    r'"([a-z]+)"',
    (REPO / "common/include/parties/bip39_wordlist.h").read_text(encoding="utf-8"),
)
assert len(WORDS) == 2048

NBSP = "\N{NO-BREAK SPACE}"
ABOUT = " ".join(["abandon"] * 11 + ["about"])


def hand_written() -> list[bytes]:
    texts = [
        # Accepted by the C++ client.
        ABOUT,
        " ".join(["abandon"] * 12),
        " ".join(["zoo"] * 12),
        ABOUT + " ",
        " " + ABOUT,
        ABOUT.replace(" ", "  ", 1),
        ABOUT.replace(" ", "\t"),
        ABOUT.replace(" ", "\n"),
        ABOUT.replace(" ", "\r\n"),
        ABOUT.replace(" ", "\v"),
        ABOUT.replace(" ", "\f"),
        "\n\n " + ABOUT + " \t\r\n",
        # Refused: count, case, spelling.
        "",
        " ",
        "abandon",
        "abandon xyz",
        ABOUT.rsplit(" ", 1)[0],
        ABOUT + " abandon",
        " ".join(["zoo"] * 24),
        ABOUT.replace("about", "About"),
        ABOUT.upper(),
        ABOUT.replace("about", "abuot"),
        # Refused: separators that are not white space in the "C" locale.
        ABOUT.replace(" ", NBSP, 1),
        ABOUT.replace(" ", "\N{IDEOGRAPHIC SPACE}"),
        ABOUT.replace(" ", "\N{EM SPACE}", 1),
        ABOUT.replace(" ", chr(0x85), 1),  # NEL, white space to Unicode only
        ABOUT.replace(" ", "\x1f", 1),
        ABOUT.replace(" ", "\x00", 1),
        ABOUT + "\x00",
        ABOUT.replace(" ", ",", 1),
        ABOUT.replace(" ", ", "),
        ABOUT.replace(" ", "-"),
        "\N{ZERO WIDTH NO-BREAK SPACE}" + ABOUT,
        ABOUT + "\N{ZERO WIDTH SPACE}",
    ]
    cases = [text.encode() for text in texts]
    # Bytes that are not UTF-8 at all.
    cases += [
        ABOUT.encode().replace(b" ", b"\xa0", 1),
        ABOUT.encode() + b"\xff",
        b"\x80" + ABOUT.encode(),
    ]
    return cases


def random_phrases(rng: random.Random, count: int) -> list[bytes]:
    separators = [b" "] * 30 + [
        b"  ", b"\t", b"\n", b"\r\n", b"\x0b", b"\x0c", b" \t ",
        NBSP.encode(), b"\x00", "\N{IDEOGRAPHIC SPACE}".encode(), b",", b"\x1c", b"\x85",
    ]
    edges = [b""] * 12 + [b" ", b"\n", b"\t", b"\r\n", b"  ", b"\x0b", b"\x0c", NBSP.encode(), b"\x00"]

    def mutate(word: str) -> bytes:
        roll = rng.random()
        if roll < 0.90:
            return word.encode()
        if roll < 0.93:
            return word.capitalize().encode()
        if roll < 0.95:
            return word.upper().encode()
        if roll < 0.97:
            return (word + rng.choice("sx1")).encode()
        if roll < 0.99:
            return word[:-1].encode()
        return word.encode() + "\N{LATIN SMALL LETTER E WITH ACUTE}".encode()

    phrases = []
    for _ in range(count):
        words = rng.choice([10, 11, 12, 12, 12, 12, 12, 12, 13, 14, 24])
        plain = rng.random() < 0.5
        parts = [b"" if plain else rng.choice(edges)]
        for index in range(words):
            if index:
                parts.append(b" " if plain and rng.random() < 0.9 else rng.choice(separators))
            word = rng.choice(WORDS)
            parts.append(word.encode() if plain and rng.random() < 0.97 else mutate(word))
        parts.append(b"" if plain else rng.choice(edges))
        phrases.append(b"".join(parts))
    return phrases


def hex_keys(rng: random.Random, count: int) -> list[bytes]:
    keys = [b"0x" + b"ab" * 31, b" " + b"ab" * 32, b"ab" * 32 + b"\n", b"", b"ab" * 32, b"AB" * 32]
    for _ in range(count):
        raw = bytes(rng.getrandbits(8) for _ in range(32)).hex()
        roll = rng.random()
        if roll < 0.4:
            text = raw
        elif roll < 0.6:
            text = raw.upper()
        elif roll < 0.7:
            text = "".join(c.upper() if rng.random() < 0.5 else c for c in raw)
        elif roll < 0.8:
            text = raw[: rng.choice([0, 1, 62, 63])]
        elif roll < 0.9:
            text = raw + rng.choice(["0", "ab", " "])
        else:
            position = rng.randrange(64)
            text = raw[:position] + rng.choice(["g", " ", "x", "-"]) + raw[position + 1 :]
        keys.append(text.encode())
    return keys


def run(binary: str, mode: str, inputs: list[bytes]) -> list[list[str]]:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "inputs.txt"
        path.write_bytes(b"".join(item.hex().encode() + b"\n" for item in inputs))
        output = subprocess.run([binary, mode, str(path)], capture_output=True, check=True).stdout
    rows = [line.split(" ") for line in output.decode("ascii").splitlines()]
    assert [row[0] for row in rows] == [item.hex() for item in inputs]
    return rows


def main() -> None:
    binary = sys.argv[1]
    rng = random.Random(20261113)

    phrases = []
    for row in run(binary, "phrase", hand_written() + random_phrases(rng, 110)):
        case = {"input": row[0], "valid": row[1] == "1"}
        if case["valid"]:
            case |= {"seed": row[2], "public_key": row[3], "fingerprint": row[4]}
        phrases.append(case)

    keys = []
    for row in run(binary, "hex", hex_keys(rng, 50)):
        case = {"input": row[0], "valid": row[1] == "1"}
        if case["valid"]:
            case["public_key"] = row[2]
        keys.append(case)

    assert sum(case["valid"] for case in phrases) >= 30
    assert sum(not case["valid"] for case in phrases) >= 30
    body = {
        "about": "Answers of the C++ client itself, recorded by generate_cpp.py from the "
        "program built from cpp_reference.cpp and common/src/crypto.cpp. 'input' is the "
        "text in hex, so that white space and bytes that are not UTF-8 survive. "
        "'phrases': valid = validate_seed_phrase(input); for the valid ones, seed and "
        "public_key = derive_keypair(input) and fingerprint = "
        "public_key_fingerprint(public_key). 'hex_keys': valid = "
        "secret_key_from_hex(input), public_key = derive_pubkey of the decoded key.",
        "phrases": phrases,
        "hex_keys": keys,
    }
    path = HERE / "cpp_v1.json"
    path.write_bytes((json.dumps(body, indent=1) + "\n").encode("ascii"))
    print(f"wrote {path.relative_to(REPO)}: {len(phrases)} phrases, {len(keys)} keys")


if __name__ == "__main__":
    main()
