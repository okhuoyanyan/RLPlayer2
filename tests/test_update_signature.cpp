#include "net/update_check.h"

#include <bcrypt.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace signature_tests {

static int passes = 0;
static int failures = 0;

static void check(bool condition, const char* name) {
    if (condition) {
        ++passes;
        std::printf("  PASS  %s\n", name);
    } else {
        ++failures;
        std::printf("  FAIL  %s\n", name);
    }
}

struct TemporaryKey {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::string modulusHex;
    unsigned long exponent = 0;

    ~TemporaryKey() {
        if (key) BCryptDestroyKey(key);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

static std::string upper_hex(const unsigned char* data, size_t size) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

static TemporaryKey generate_key() {
    TemporaryKey out;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&out.algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0);
    if (status < 0) throw std::runtime_error("BCryptOpenAlgorithmProvider");
    status = BCryptGenerateKeyPair(out.algorithm, &out.key, 3072, 0);
    if (status < 0) throw std::runtime_error("BCryptGenerateKeyPair");
    status = BCryptFinalizeKeyPair(out.key, 0);
    if (status < 0) throw std::runtime_error("BCryptFinalizeKeyPair");
    DWORD bytes = 0;
    status = BCryptExportKey(out.key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &bytes, 0);
    if (status < 0) throw std::runtime_error("BCryptExportKey size");
    std::vector<unsigned char> blob(bytes);
    status = BCryptExportKey(out.key, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(), bytes, &bytes, 0);
    if (status < 0) throw std::runtime_error("BCryptExportKey");
    const auto* header = reinterpret_cast<const BCRYPT_RSAKEY_BLOB*>(blob.data());
    if (header->cbPublicExp == 0 || header->cbPublicExp > 4 || header->cbModulus != 384)
        throw std::runtime_error("unexpected RSA public blob");
    unsigned long exponent = 0;
    for (DWORD i = 0; i < header->cbPublicExp; ++i)
        exponent = (exponent << 8) | blob[sizeof(BCRYPT_RSAKEY_BLOB) + i];
    out.exponent = exponent;
    out.modulusHex = upper_hex(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB) + header->cbPublicExp,
                               header->cbModulus);
    return out;
}

static std::vector<unsigned char> sha256(BCRYPT_ALG_HANDLE, const std::string& input) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD failed = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("BCryptCreateHash");
    std::vector<unsigned char> digest(32);
    const NTSTATUS status = BCryptHashData(hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), (ULONG)input.size(), 0);
    if (status >= 0) {
        if (BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0) < 0) failed = 1;
    } else {
        failed = 1;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (failed) throw std::runtime_error("BCrypt hash data");
    return digest;
}

static std::string sign_envelope(TemporaryKey& key, const std::string& payload,
                                 const std::string& kid) {
    const std::string protectedJson = "{\"alg\":\"PS256\",\"kid\":\"" + kid + "\"}";
    const std::string protectedPart = rta::update::base64url_encode(protectedJson);
    const std::string payloadPart = rta::update::base64url_encode(payload);
    const std::string input = protectedPart + "." + payloadPart;
    const auto digest = sha256(key.algorithm, input);
    BCRYPT_PSS_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, 32};
    DWORD bytes = 0;
    NTSTATUS status = BCryptSignHash(key.key, &padding,
        const_cast<PUCHAR>(digest.data()), (ULONG)digest.size(), nullptr, 0, &bytes, BCRYPT_PAD_PSS);
    if (status < 0) throw std::runtime_error("BCryptSignHash size");
    std::vector<unsigned char> signature(bytes);
    status = BCryptSignHash(key.key, &padding,
        const_cast<PUCHAR>(digest.data()), (ULONG)digest.size(), signature.data(), bytes, &bytes,
        BCRYPT_PAD_PSS);
    if (status < 0) throw std::runtime_error("BCryptSignHash");
    const std::string signaturePart = rta::update::base64url_encode(
        reinterpret_cast<const char*>(signature.data()), signature.size());
    return "{\"alg\":\"PS256\",\"kid\":\"" + kid + "\",\"payload\":\"" + payloadPart +
           "\",\"signature\":\"" + signaturePart + "\"}";
}

static int run() {
    passes = failures = 0;
    std::printf("== CNG RSA-PSS-SHA256 signed update envelope ==\n");
    check(std::string(rta::update::kWindowsUpdatePublicModulusHex).size() == 768,
          "production RSA modulus is 3072 bits");
    check(rta::update::kWindowsUpdatePublicExponent == 65537, "production exponent is 65537");
    check(rta::update::base64url_encode(std::string("f")) == "Zg", "base64url unpadded vector f");
    check(rta::update::base64url_encode(std::string("fo")) == "Zm8", "base64url unpadded vector fo");
    check(rta::update::base64url_encode(std::string("foo")) == "Zm9v", "base64url vector foo");
    check(rta::update::base64url_encode(std::string("foob")) == "Zm9vYg", "base64url vector foob");
    std::string decoded;
    check(!rta::update::base64url_decode("Zg==", decoded), "base64url padding rejected");
    check(!rta::update::base64url_decode("Zg+", decoded), "base64 alphabet rejected");
    check(!rta::update::base64url_decode("Zh", decoded), "non-canonical trailing bits rejected");

    auto key = generate_key();
    rta::update::UpdateSignatureKey publicKey;
    publicKey.keyId = "temporary-test-key";
    publicKey.modulusHex = key.modulusHex;
    publicKey.exponent = key.exponent;
    const std::string payload = "{\"schema\":2,\"version\":\"3.2.2\",\"notes\":\"原始 JSON\\n\"}";
    const std::string envelope = sign_envelope(key, payload, publicKey.keyId);
    auto verified = rta::update::verify_windows_update_envelope(envelope, publicKey);
    check(verified.ok, "real CNG 3072-bit RSA-PSS signature accepted");
    check(verified.ok && verified.payload == payload, "verified payload is byte-for-byte original JSON");
    check(verified.ok && verified.kid == publicKey.keyId, "verified key id returned");

    std::string changedPayload = payload;
    changedPayload[changedPayload.find("3.2.2") + 5] = '3';
    const std::string changedEnvelope =
        "{\"alg\":\"PS256\",\"kid\":\"temporary-test-key\",\"payload\":\"" +
        rta::update::base64url_encode(changedPayload) + "\",\"signature\":\"" +
        rta::net::json_parse(envelope)["signature"].s() + "\"}";
    check(!rta::update::verify_windows_update_envelope(changedEnvelope, publicKey).ok,
          "payload mutation rejected");

    const std::string originalSignature = rta::net::json_parse(envelope)["signature"].s();
    std::string badSignature = originalSignature;
    badSignature[10] = badSignature[10] == 'A' ? 'B' : 'A';
    const std::string signatureEnvelope =
        "{\"alg\":\"PS256\",\"kid\":\"temporary-test-key\",\"payload\":\"" +
        rta::update::base64url_encode(payload) + "\",\"signature\":\"" + badSignature + "\"}";
    check(!rta::update::verify_windows_update_envelope(signatureEnvelope, publicKey).ok,
          "signature mutation rejected");

    const std::string wrongKidEnvelope = sign_envelope(key, payload, "wrong-key");
    check(!rta::update::verify_windows_update_envelope(wrongKidEnvelope, publicKey).ok,
          "kid mismatch rejected");
    check(!rta::update::verify_windows_update_envelope(
              sign_envelope(key, payload, publicKey.keyId).replace(2, 4, "RS25"), publicKey).ok,
          "non-PS256 algorithm rejected");
    check(!rta::update::verify_windows_update_envelope("{}", publicKey).ok,
          "unsigned envelope rejected");
    check(!rta::update::verify_windows_update_envelope(envelope + "junk", publicKey).ok,
          "trailing envelope data rejected");

    auto wrongKey = publicKey;
    wrongKey.modulusHex[0] = wrongKey.modulusHex[0] == '0' ? '1' : '0';
    check(!rta::update::verify_windows_update_envelope(envelope, wrongKey).ok,
          "different public key rejected");

    auto decisionEnvelope = [&](const std::string& version, const std::string& expires) {
        const std::string raw = rta::update::sha256_hex(std::string("NEW"));
        const std::string zip = rta::update::sha256_hex(std::string("ZIP"));
        const std::string manifest = "{\"schema\":2,\"platform\":\"windows\",\"channel\":\"stable\","
            "\"metadata_version\":1,\"version\":\"" + version + "\","
            "\"published_at\":\"2026-11-01T00:00:00Z\",\"expires_at\":\"" + expires + "\","
            "\"install_mode\":\"incremental\",\"reason\":\"signed decision\","
            "\"files\":[{\"path\":\"app.exe\",\"sha256\":\"" + raw + "\",\"size\":3,"
            "\"object\":\"" + raw + ".zip\",\"object_sha256\":\"" + zip + "\",\"object_size\":3}],"
            "\"delete\":[],\"manual\":{\"cn_url\":\"https://pan.baidu.com/s/test\","
            "\"global_url\":\"https://github.com/okhuoyanyan/RLPlayer2/releases\","
            "\"extraction_code\":\"abcd\",\"sha256\":\"" + zip + "\",\"size\":12345},"
            "\"notes\":\"signed\"}";
        return sign_envelope(key, manifest, publicKey.keyId);
    };
    const long long now = rta::update::parse_rfc3339_epoch("2026-11-03T00:00:00Z");
    const std::string decision = decisionEnvelope("3.2.3", "2027-01-01T00:00:00Z");
    check(rta::update::evaluate_update_check("3.2.2", true, 200, decision, now, "stable", publicKey).decision ==
              rta::update::Decision::UpdateAvailable,
          "verified signature then strict payload produces update decision");
    check(rta::update::evaluate_update_check("3.2.2", true, 200,
              decisionEnvelope("3.2.3", "2026-11-02T00:00:00Z"), now, "stable", publicKey).decision ==
              rta::update::Decision::FailedExpired,
          "validly signed expired payload rejected by full decision path");
    check(rta::update::evaluate_update_check("3.2.3", true, 200,
              decisionEnvelope("3.2.2", "2027-01-01T00:00:00Z"), now, "stable", publicKey).decision ==
              rta::update::Decision::FailedRollback,
          "validly signed rollback rejected by full decision path");
    std::string expiredTamper = rta::update::verify_windows_update_envelope(decision, publicKey).payload;
    expiredTamper.replace(expiredTamper.find("2027-01-01"), 10, "2020-01-01");
    const std::string expiredTamperEnvelope =
        "{\"alg\":\"PS256\",\"kid\":\"temporary-test-key\",\"payload\":\"" +
        rta::update::base64url_encode(expiredTamper) + "\",\"signature\":\"" +
        rta::net::json_parse(decision)["signature"].s() + "\"}";
    check(rta::update::evaluate_update_check("3.2.2", true, 200, expiredTamperEnvelope,
              now, "stable", publicKey).decision == rta::update::Decision::FailedSignature,
          "expiry tampering without re-signing fails before payload policy");
    std::printf("signature tests: PASS %d / FAIL %d\n", passes, failures);
    return failures;
}

} // namespace signature_tests

#ifdef RLPLAYER_UPDATE_SIGNATURE_TEST_MAIN
int main() {
    try {
        return signature_tests::run() == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "signature test setup failed: %s\n", e.what());
        return 2;
    }
}
#endif
