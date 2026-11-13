// Runs the identity functions of the C++ client (common/src/crypto.cpp) over
// a list of inputs. generate_cpp.py turns its output into cpp_v1.json, the
// vectors that tie derivation v1 of parties-identity to the C++ code itself.
//
// Build, from the repository root, with the OpenSSL the C++ tree is built
// with (vcpkg, triplet x64-win-llvm); any compiler and OpenSSL 3 will do:
//
//   clang-cl /std:c++latest /EHsc /MT /I common\include
//       /I <vcpkg_installed>\x64-win-llvm\include
//       crates\parties-identity\tests\vectors\cpp_reference.cpp
//       common\src\crypto.cpp /Fe:cpp_reference.exe
//       /link /LIBPATH:<vcpkg_installed>\x64-win-llvm\lib libcrypto.lib
//       libssl.lib crypt32.lib ws2_32.lib advapi32.lib user32.lib bcrypt.lib
//
// Usage: cpp_reference phrase|hex <file>
//
// The file has one input per line, hex-encoded so that white space survives.
//
//   phrase: validate_seed_phrase, derive_keypair, public_key_fingerprint.
//           Prints: <input hex> <valid 0|1> <seed hex> <public key hex> <fingerprint>
//   hex:    secret_key_from_hex, derive_pubkey.
//           Prints: <input hex> <ok 0|1> <public key hex, or ->

#include <parties/crypto.h>

#include <fstream>
#include <iostream>
#include <string>

static std::string from_hex(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

template <typename Bytes>
static std::string to_hex(const Bytes& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) {
        const auto u = static_cast<unsigned char>(b);
        out.push_back(digits[u >> 4]);
        out.push_back(digits[u & 0xF]);
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: cpp_reference phrase|hex <file>\n";
        return 2;
    }
    const std::string mode = argv[1];
    std::ifstream in(argv[2]);
    parties::crypto_init();

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string input = from_hex(line);
        parties::SecretKey sk{};
        parties::PublicKey pk{};
        if (mode == "phrase") {
            const bool valid = parties::validate_seed_phrase(input);
            // derive_keypair does not validate; the client calls it after
            // validate_seed_phrase. Here it runs for every input.
            parties::derive_keypair(input, sk, pk);
            std::cout << line << ' ' << (valid ? 1 : 0) << ' ' << to_hex(sk) << ' ' << to_hex(pk)
                      << ' ' << parties::public_key_fingerprint(pk) << '\n';
        } else {
            const bool ok = parties::secret_key_from_hex(input, sk) && parties::derive_pubkey(sk, pk);
            std::cout << line << ' ' << (ok ? 1 : 0) << ' ' << (ok ? to_hex(pk) : std::string("-"))
                      << '\n';
        }
    }
    return 0;
}
