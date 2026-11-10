// Windows update envelope authentication using the operating system CNG provider.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "net/json.h"
#include "net/update_public_key.h"

#pragma comment(lib, "bcrypt.lib")

namespace rta::update {

struct UpdateSignatureKey {
    std::string keyId = kWindowsUpdateKeyId;
    std::string modulusHex = kWindowsUpdatePublicModulusHex;
    unsigned long exponent = kWindowsUpdatePublicExponent;
};

struct VerifiedUpdateEnvelope {
    bool ok = false;
    std::string error;
    std::string errorCode;
    std::string kid;
    std::string payload;
};

inline std::string base64url_encode(const char* data, size_t size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= size) {
        const uint32_t value = ((uint32_t)(unsigned char)data[i] << 16) |
                               ((uint32_t)(unsigned char)data[i + 1] << 8) |
                               (uint32_t)(unsigned char)data[i + 2];
        out.push_back(alphabet[(value >> 18) & 63]);
        out.push_back(alphabet[(value >> 12) & 63]);
        out.push_back(alphabet[(value >> 6) & 63]);
        out.push_back(alphabet[value & 63]);
        i += 3;
    }
    if (i < size) {
        const uint32_t value = (uint32_t)(unsigned char)data[i] << 16 |
            (i + 1 < size ? ((uint32_t)(unsigned char)data[i + 1] << 8) : 0);
        out.push_back(alphabet[(value >> 18) & 63]);
        out.push_back(alphabet[(value >> 12) & 63]);
        if (i + 1 < size) out.push_back(alphabet[(value >> 6) & 63]);
    }
    return out;
}

inline std::string base64url_encode(const std::string& text) {
    return base64url_encode(text.data(), text.size());
}

inline bool base64url_decode(const std::string& encoded, std::string& out, size_t maxBytes = 4 * 1024 * 1024) {
    out.clear();
    if (encoded.empty() || encoded.size() > (maxBytes + 2) / 3 * 4 + 2 || encoded.size() % 4 == 1)
        return false;
    auto value = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    std::vector<unsigned char> bytes;
    bytes.reserve(encoded.size() / 4 * 3 + 2);
    size_t i = 0;
    while (i + 4 <= encoded.size()) {
        uint32_t v = 0;
        for (size_t n = 0; n < 4; ++n) {
            const int digit = value((unsigned char)encoded[i + n]);
            if (digit < 0) return false;
            v = (v << 6) | (unsigned)digit;
        }
        bytes.push_back((unsigned char)(v >> 16));
        bytes.push_back((unsigned char)(v >> 8));
        bytes.push_back((unsigned char)v);
        i += 4;
    }
    const size_t left = encoded.size() - i;
    if (left == 2) {
        const int a = value((unsigned char)encoded[i]);
        const int b = value((unsigned char)encoded[i + 1]);
        if (a < 0 || b < 0 || (b & 15) != 0) return false;
        bytes.push_back((unsigned char)((a << 2) | (b >> 4)));
    } else if (left == 3) {
        const int a = value((unsigned char)encoded[i]);
        const int b = value((unsigned char)encoded[i + 1]);
        const int c = value((unsigned char)encoded[i + 2]);
        if (a < 0 || b < 0 || c < 0 || (c & 3) != 0) return false;
        bytes.push_back((unsigned char)((a << 2) | (b >> 4)));
        bytes.push_back((unsigned char)((b << 4) | (c >> 2)));
    } else if (left != 0) {
        return false;
    }
    if (bytes.size() > maxBytes) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return base64url_encode(out) == encoded;
}

namespace update_signature_detail {

inline bool hex_byte(char a, char b, unsigned char& out) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const int x = digit(a), y = digit(b);
    if (x < 0 || y < 0) return false;
    out = (unsigned char)((x << 4) | y);
    return true;
}

inline bool cng_sha256(const std::string& input, unsigned char digest[32]) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status >= 0)
        status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                                (ULONG)input.size(), 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest, 32, 0);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return status >= 0;
}

} // namespace update_signature_detail

inline bool verify_rsa_pss_sha256(const std::string& modulusHex, unsigned long exponent,
                                  const std::string& signingInput, const std::string& signature,
                                  std::string& error) {
    error.clear();
    if (modulusHex.size() != 768 || exponent < 3 || !(exponent & 1) || signature.size() != 384) {
        error = "RSA-3072 public key or signature shape is invalid";
        return false;
    }
    std::vector<unsigned char> modulus(384);
    for (size_t i = 0; i < modulus.size(); ++i) {
        if (!update_signature_detail::hex_byte(modulusHex[i * 2], modulusHex[i * 2 + 1], modulus[i])) {
            error = "RSA public modulus is not hexadecimal";
            return false;
        }
    }
    unsigned char exponentBytes[4] = {
        (unsigned char)(exponent >> 24), (unsigned char)(exponent >> 16),
        (unsigned char)(exponent >> 8), (unsigned char)exponent
    };
    size_t exponentSize = 4;
    while (exponentSize > 1 && exponentBytes[4 - exponentSize] == 0) --exponentSize;
    const size_t exponentOffset = 4 - exponentSize;

    std::vector<unsigned char> blobBytes(sizeof(BCRYPT_RSAKEY_BLOB) + exponentSize + modulus.size());
    auto* blob = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blobBytes.data());
    blob->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    blob->BitLength = 3072;
    blob->cbPublicExp = (ULONG)exponentSize;
    blob->cbModulus = (ULONG)modulus.size();
    blob->cbPrime1 = blob->cbPrime2 = 0;
    std::memcpy(blobBytes.data() + sizeof(BCRYPT_RSAKEY_BLOB), exponentBytes + exponentOffset, exponentSize);
    std::memcpy(blobBytes.data() + sizeof(BCRYPT_RSAKEY_BLOB) + exponentSize,
                modulus.data(), modulus.size());

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0);
    if (status >= 0)
        status = BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
            blobBytes.data(), (ULONG)blobBytes.size(), 0);
    unsigned char digest[32]{};
    if (status >= 0 && !update_signature_detail::cng_sha256(signingInput, digest))
        status = (NTSTATUS)-1;
    if (status >= 0) {
        BCRYPT_PSS_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, 32};
        status = BCryptVerifySignature(key, &padding, digest, sizeof(digest),
            reinterpret_cast<PUCHAR>(const_cast<char*>(signature.data())), (ULONG)signature.size(),
            BCRYPT_PAD_PSS);
    }
    if (key) BCryptDestroyKey(key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        error = "RSA-PSS-SHA256 verification failed";
        return false;
    }
    return true;
}

inline VerifiedUpdateEnvelope verify_windows_update_envelope(
    const std::string& envelope, const UpdateSignatureKey& key = UpdateSignatureKey{}) {
    VerifiedUpdateEnvelope out;
    auto fail = [&](const char* code, const std::string& message) {
        out.ok = false; out.errorCode = code; out.error = message; out.payload.clear(); return out;
    };
    if (envelope.empty() || envelope.size() > 4 * 1024 * 1024)
        return fail("envelope-size", "signed update envelope size is invalid");
    rta::net::Json json;
    if (!rta::net::json_parse_strict(envelope, json) || json.type != rta::net::Json::Obj)
        return fail("envelope-json", "signed update envelope is not strict JSON");
    for (const auto& item : json.obj) {
        if (item.first != "alg" && item.first != "kid" && item.first != "payload" &&
            item.first != "signature")
            return fail("envelope-field", "unknown signed envelope field");
    }
    if (json["alg"].type != rta::net::Json::Str || json["kid"].type != rta::net::Json::Str ||
        json["payload"].type != rta::net::Json::Str || json["signature"].type != rta::net::Json::Str)
        return fail("envelope-field", "signed envelope fields have wrong types");
    if (json["alg"].str != "PS256") return fail("algorithm", "only JWS PS256 is accepted");
    const std::string& kid = json["kid"].str;
    if (kid.empty() || kid.size() > 128 || kid != key.keyId)
        return fail("key-id", "signed update key id does not match");
    for (unsigned char c : kid) {
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != ':')
            return fail("key-id", "signed update key id has invalid characters");
    }
    std::string payload, signature;
    if (!base64url_decode(json["payload"].str, payload, 2 * 1024 * 1024))
        return fail("payload-encoding", "JWS payload is not canonical base64url");
    if (!base64url_decode(json["signature"].str, signature, 384) || signature.size() != 384)
        return fail("signature-encoding", "JWS signature is not a 3072-bit base64url value");

    const std::string protectedJson = "{\"alg\":\"PS256\",\"kid\":\"" + kid + "\"}";
    const std::string signingInput = base64url_encode(protectedJson) + "." + json["payload"].str;
    std::string cryptoError;
    if (!verify_rsa_pss_sha256(key.modulusHex, key.exponent, signingInput, signature, cryptoError))
        return fail("signature", cryptoError);
    out.ok = true;
    out.kid = kid;
    out.payload = std::move(payload);
    return out;
}

} // namespace rta::update
