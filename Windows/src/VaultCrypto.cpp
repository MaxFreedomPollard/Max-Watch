#include "VaultCrypto.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#else
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <Security/SecRandom.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace maxwatch {

namespace {

constexpr uint8_t kMagicBytes[4] = {0x4D, 0x57, 0x56, 0x31}; // "MWV1"
constexpr const char* kWrapAAD = "MaxWatchVaultKeyV1";
constexpr const char* kClipHKDFInfo = "MaxWatchClipAES256GCM_V1";
constexpr const char* kChunkAADPrefix = "MWV1Chunk";

void writeBE16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}
void writeBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}
void writeBE64(std::vector<uint8_t>& out, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
    }
}
uint16_t readBE16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}
uint64_t readBE64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    }
    return v;
}

#ifndef _WIN32
// Portable reference AES-256 + GCM solely for macOS/POSIX local cross-verification tests.
// On Windows (_WIN32), VaultCrypto uses hardware-accelerated Windows CNG (bcrypt.dll).
namespace ref_aes_gcm {

static const uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ (((x >> 7) & 1) * 0x1b)); }

struct Aes256Ctx {
    uint8_t roundKey[240];
    explicit Aes256Ctx(const uint8_t key[32]) {
        static const uint8_t Rcon[11] = {0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
        std::memcpy(roundKey, key, 32);
        uint8_t tempa[4];
        for (int i = 8; i < 60; ++i) {
            for (int k = 0; k < 4; ++k) tempa[k] = roundKey[(i - 1) * 4 + k];
            if (i % 8 == 0) {
                uint8_t u = tempa[0];
                tempa[0] = kSbox[tempa[1]] ^ Rcon[i / 8];
                tempa[1] = kSbox[tempa[2]];
                tempa[2] = kSbox[tempa[3]];
                tempa[3] = kSbox[u];
            } else if (i % 8 == 4) {
                for (int k = 0; k < 4; ++k) tempa[k] = kSbox[tempa[k]];
            }
            for (int k = 0; k < 4; ++k) roundKey[i * 4 + k] = roundKey[(i - 8) * 4 + k] ^ tempa[k];
        }
    }
    ~Aes256Ctx() { crypto::secureZero(roundKey, sizeof(roundKey)); }

    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        uint8_t s[16];
        for (int i = 0; i < 16; ++i) s[i] = in[i] ^ roundKey[i];
        for (int r = 1; r <= 14; ++r) {
            for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];
            uint8_t t;
            t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
            t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
            t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
            if (r < 14) {
                for (int c = 0; c < 4; ++c) {
                    int idx = c * 4;
                    uint8_t a0 = s[idx], a1 = s[idx+1], a2 = s[idx+2], a3 = s[idx+3];
                    uint8_t tm = a0 ^ a1 ^ a2 ^ a3;
                    s[idx]   ^= tm ^ xtime(a0 ^ a1);
                    s[idx+1] ^= tm ^ xtime(a1 ^ a2);
                    s[idx+2] ^= tm ^ xtime(a2 ^ a3);
                    s[idx+3] ^= tm ^ xtime(a3 ^ a0);
                }
            }
            for (int i = 0; i < 16; ++i) s[i] ^= roundKey[r * 16 + i];
        }
        std::memcpy(out, s, 16);
    }
};

static void ghashMul(uint8_t X[16], const uint8_t H[16]) {
    uint8_t Z[16] = {0};
    uint8_t V[16];
    std::memcpy(V, H, 16);
    for (int i = 0; i < 128; ++i) {
        if ((X[i >> 3] >> (7 - (i & 7))) & 1) {
            for (int b = 0; b < 16; ++b) Z[b] ^= V[b];
        }
        uint8_t lsb = V[15] & 1;
        for (int b = 15; b > 0; --b) V[b] = static_cast<uint8_t>((V[b] >> 1) | ((V[b - 1] & 1) << 7));
        V[0] >>= 1;
        if (lsb) V[0] ^= 0xE1;
    }
    std::memcpy(X, Z, 16);
}

static void ghashCompute(const uint8_t H[16], const uint8_t* aad, size_t aadLen, const uint8_t* ct, size_t ctLen, uint8_t out[16]) {
    uint8_t X[16] = {0};
    auto feed = [&](const uint8_t* buf, size_t len) {
        size_t off = 0;
        while (off < len) {
            size_t take = std::min<size_t>(16, len - off);
            for (size_t i = 0; i < take; ++i) X[i] ^= buf[off + i];
            ghashMul(X, H);
            off += take;
        }
    };
    feed(aad, aadLen);
    feed(ct, ctLen);
    uint8_t lenBlk[16] = {0};
    uint64_t aadBits = static_cast<uint64_t>(aadLen) * 8ULL;
    uint64_t ctBits = static_cast<uint64_t>(ctLen) * 8ULL;
    for (int i = 0; i < 8; ++i) {
        lenBlk[i] = static_cast<uint8_t>((aadBits >> (56 - i * 8)) & 0xFF);
        lenBlk[8 + i] = static_cast<uint8_t>((ctBits >> (56 - i * 8)) & 0xFF);
    }
    for (int i = 0; i < 16; ++i) X[i] ^= lenBlk[i];
    ghashMul(X, H);
    std::memcpy(out, X, 16);
}

static void inc32(uint8_t ctr[16]) {
    uint32_t c = readBE32(ctr + 12) + 1;
    ctr[12] = static_cast<uint8_t>((c >> 24) & 0xFF);
    ctr[13] = static_cast<uint8_t>((c >> 16) & 0xFF);
    ctr[14] = static_cast<uint8_t>((c >> 8) & 0xFF);
    ctr[15] = static_cast<uint8_t>(c & 0xFF);
}

} // namespace ref_aes_gcm
#endif

// Constant-Time RFC 7748 X25519 (TweetNaCl GF(2^255-19) Montgomery ladder)
using gf25519 = int64_t[16];

void car25519(gf25519 o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

void sel25519(gf25519 p, gf25519 q, int b) {
    int64_t c = ~(static_cast<int64_t>(b) - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack25519(uint8_t* o, const gf25519 n) {
    gf25519 m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<uint8_t>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<uint8_t>(t[i] >> 8);
    }
}

void unpack25519(gf25519 o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) {
        o[i] = static_cast<int64_t>(n[2 * i]) + (static_cast<int64_t>(n[2 * i + 1]) << 8);
    }
    o[15] &= 0x7fff;
}

void add25519(gf25519 o, const gf25519 a, const gf25519 b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}
void sub25519(gf25519 o, const gf25519 a, const gf25519 b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}
void mul25519(gf25519 o, const gf25519 a, const gf25519 b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}
void sqr25519(gf25519 o, const gf25519 a) { mul25519(o, a, a); }
void inv25519(gf25519 o, const gf25519 in) {
    gf25519 c;
    for (int a = 0; a < 16; ++a) c[a] = in[a];
    for (int a = 253; a >= 0; --a) {
        sqr25519(c, c);
        if (a != 2 && a != 4) mul25519(c, c, in);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void scalarmult25519(uint8_t q[32], const uint8_t n[32], const uint8_t p[32]) {
    uint8_t z[32];
    int64_t x[80];
    gf25519 a, b, c, d, e, f;
    static const gf25519 k121665 = {0xDB41, 1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    std::memcpy(z, n, 32);
    z[31] = static_cast<uint8_t>((z[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        add25519(e, a, c);
        sub25519(a, a, c);
        add25519(c, b, d);
        sub25519(b, b, d);
        sqr25519(d, e);
        sqr25519(f, a);
        mul25519(a, c, a);
        mul25519(c, b, e);
        add25519(e, a, c);
        sub25519(a, a, c);
        sqr25519(b, a);
        sub25519(c, d, f);
        mul25519(a, c, k121665);
        add25519(a, a, d);
        mul25519(c, c, a);
        mul25519(a, d, f);
        mul25519(d, b, x);
        sqr25519(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    for (int i = 0; i < 16; ++i) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv25519(x + 32, x + 32);
    mul25519(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
    crypto::secureZero(z, sizeof(z));
    crypto::secureZero(x, sizeof(x));
}

std::string currentISO8601UTC() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm gmt{};
#ifdef _WIN32
    gmtime_s(&gmt, &tt);
#else
    gmtime_r(&tt, &gmt);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gmt);
    return std::string(buf);
}

std::string extractJsonStringField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Missing field in vault_manifest.json: " + key);
    }
    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Malformed vault_manifest.json");
    }
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Expected string value in vault_manifest.json");
    }
    std::string out;
    for (size_t i = q1 + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (ch == '\\' && i + 1 < json.size()) {
            char next = json[++i];
            if (next == '"' || next == '\\' || next == '/') out.push_back(next);
            else if (next == 'n') out.push_back('\n');
            else if (next == 't') out.push_back('\t');
            else out.push_back(next);
        } else if (ch == '"') {
            return out;
        } else {
            out.push_back(ch);
        }
    }
    throw VaultException(VaultErrorKind::CorruptedManifest, "Unterminated string in vault_manifest.json");
}

int extractJsonIntField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Missing integer field in vault_manifest.json: " + key);
    }
    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Malformed vault_manifest.json");
    }
    size_t p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
    size_t end = p;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (end == p) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Invalid integer in vault_manifest.json");
    }
    return std::stoi(json.substr(p, end - p));
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out.push_back(c);
    }
    return out;
}

} // namespace

// MARK: - UUIDBytes

UUIDBytes UUIDBytes::generateRandom() {
    UUIDBytes u;
    crypto::randomBytes(u.bytes.data(), u.bytes.size());
    // RFC 4122 v4 UUID bits
    u.bytes[6] = static_cast<uint8_t>((u.bytes[6] & 0x0F) | 0x40);
    u.bytes[8] = static_cast<uint8_t>((u.bytes[8] & 0x3F) | 0x80);
    return u;
}

std::string UUIDBytes::toString() const {
    char buf[40];
    std::snprintf(
        buf, sizeof(buf),
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5],
        bytes[6], bytes[7],
        bytes[8], bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]
    );
    return std::string(buf);
}

std::string UUIDBytes::shortPrefix8() const {
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3]);
    return std::string(buf);
}

// MARK: - VaultManifest

std::string VaultManifest::toJson() const {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"createdAtISO8601\" : \"" << jsonEscape(createdAtISO8601) << "\",\n"
        << "  \"pbkdf2Iterations\" : " << pbkdf2Iterations << ",\n"
        << "  \"pbkdf2SaltBase64\" : \"" << jsonEscape(pbkdf2SaltBase64) << "\",\n"
        << "  \"publicKeyBase64\" : \"" << jsonEscape(publicKeyBase64) << "\",\n"
        << "  \"version\" : " << version << ",\n"
        << "  \"wrappedPrivateKeyBase64\" : \"" << jsonEscape(wrappedPrivateKeyBase64) << "\"\n"
        << "}";
    return oss.str();
}

VaultManifest VaultManifest::fromJson(const std::string& jsonText) {
    VaultManifest m;
    m.version = extractJsonIntField(jsonText, "version");
    m.pbkdf2Iterations = extractJsonIntField(jsonText, "pbkdf2Iterations");
    m.pbkdf2SaltBase64 = extractJsonStringField(jsonText, "pbkdf2SaltBase64");
    m.publicKeyBase64 = extractJsonStringField(jsonText, "publicKeyBase64");
    m.wrappedPrivateKeyBase64 = extractJsonStringField(jsonText, "wrappedPrivateKeyBase64");
    m.createdAtISO8601 = extractJsonStringField(jsonText, "createdAtISO8601");
    if (m.version != 1 || m.pbkdf2Iterations < 10000) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Unsupported vault manifest parameters.");
    }
    return m;
}

// MARK: - ClipFileHeader (Exact 128-byte MWV1 layout)

std::vector<uint8_t> ClipFileHeader::serialize() const {
    std::vector<uint8_t> data;
    data.reserve(kByteSize);
    data.insert(data.end(), std::begin(kMagicBytes), std::end(kMagicBytes));
    writeBE16(data, version);
    data.push_back(flags);
    data.push_back(0); // reserved
    data.insert(data.end(), clipUUID.bytes.begin(), clipUUID.bytes.end());
    writeBE64(data, static_cast<uint64_t>(createdUnixMs));
    writeBE32(data, width);
    writeBE32(data, height);
    data.insert(data.end(), ephemeralPublicKeyRaw.begin(), ephemeralPublicKeyRaw.end());
    data.insert(data.end(), hkdfSalt.begin(), hkdfSalt.end());

    // Bytes [0..103] -> SHA256 prefix 16 bytes at [104..119]
    auto digest = crypto::sha256(data.data(), 104);
    data.insert(data.end(), digest.begin(), digest.begin() + 16);
    writeBE64(data, durationMs);
    return data;
}

ClipFileHeader ClipFileHeader::parse(const uint8_t* data, size_t len) {
    if (!data || len < kByteSize) {
        throw VaultException(VaultErrorKind::CorruptedClipHeader, "Encrypted clip header is unreadable or truncated.");
    }
    if (std::memcmp(data, kMagicBytes, 4) != 0) {
        throw VaultException(VaultErrorKind::CorruptedClipHeader, "Invalid MWV1 header magic.");
    }
    auto digest = crypto::sha256(data, 104);
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= (digest[i] ^ data[104 + i]);
    if (diff != 0) {
        throw VaultException(VaultErrorKind::CorruptedClipHeader, "Clip header SHA-256 integrity check failed.");
    }
    uint16_t ver = readBE16(data + 4);
    if (ver != kCurrentVersion) {
        throw VaultException(VaultErrorKind::CorruptedClipHeader, "Unsupported MWV1 clip version.");
    }

    ClipFileHeader h;
    h.version = ver;
    h.flags = data[6];
    std::memcpy(h.clipUUID.bytes.data(), data + 8, 16);
    h.createdUnixMs = static_cast<int64_t>(readBE64(data + 24));
    h.width = readBE32(data + 32);
    h.height = readBE32(data + 36);
    std::memcpy(h.ephemeralPublicKeyRaw.data(), data + 40, 32);
    std::memcpy(h.hkdfSalt.data(), data + 72, 32);
    h.durationMs = readBE64(data + 120);
    return h;
}

// MARK: - Cryptographic Primitives

namespace crypto {

void secureZero(void* ptr, size_t len) {
    if (!ptr || len == 0) return;
#ifdef _WIN32
    SecureZeroMemory(ptr, len);
#else
    volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
    while (len--) *p++ = 0;
#endif
}

void secureZeroVec(std::vector<uint8_t>& v) {
    if (!v.empty()) {
        secureZero(v.data(), v.size());
        v.clear();
    }
}

void randomBytes(uint8_t* out, size_t len) {
    if (len == 0) return;
#ifdef _WIN32
    NTSTATUS st = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "BCryptGenRandom failed.");
    }
#else
    if (SecRandomCopyBytes(kSecRandomDefault, len, out) != errSecSuccess) {
        throw VaultException(VaultErrorKind::CryptoFailure, "SecRandomCopyBytes failed.");
    }
#endif
}

std::vector<uint8_t> randomBytesVec(size_t len) {
    std::vector<uint8_t> v(len);
    randomBytes(v.data(), len);
    return v;
}

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len) {
    std::array<uint8_t, 32> out{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "Cannot open SHA256 provider.");
    }
    uint8_t dummy = 0;
    NTSTATUS st = BCryptHash(
        hAlg, nullptr, 0,
        len > 0 ? const_cast<uint8_t*>(data) : &dummy,
        static_cast<ULONG>(len),
        out.data(), 32
    );
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "BCryptHash SHA256 failed.");
    }
#else
    CC_SHA256(data, static_cast<CC_LONG>(len), out.data());
#endif
    return out;
}

std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen) {
    std::array<uint8_t, 32> out{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "Cannot open HMAC-SHA256 provider.");
    }
    uint8_t dummyK = 0, dummyM = 0;
    NTSTATUS st = BCryptHash(
        hAlg,
        keyLen > 0 ? const_cast<uint8_t*>(key) : &dummyK,
        static_cast<ULONG>(keyLen),
        msgLen > 0 ? const_cast<uint8_t*>(msg) : &dummyM,
        static_cast<ULONG>(msgLen),
        out.data(), 32
    );
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "BCryptHash HMAC-SHA256 failed.");
    }
#else
    CCHmac(kCCHmacAlgSHA256, key, keyLen, msg, msgLen, out.data());
#endif
    return out;
}

std::array<uint8_t, 32> derivePBKDF2Sha256(const std::string& password, const uint8_t* salt, size_t saltLen, int iterations) {
    if (password.empty()) {
        throw VaultException(VaultErrorKind::PasswordTooShort, "Password must be at least 4 characters.");
    }
    std::array<uint8_t, 32> derived{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "Cannot open PBKDF2 HMAC-SHA256 provider.");
    }
    NTSTATUS st = BCryptDeriveKeyPBKDF2(
        hAlg,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt),
        static_cast<ULONG>(saltLen),
        static_cast<ULONGLONG>(iterations),
        derived.data(),
        32,
        0
    );
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "BCryptDeriveKeyPBKDF2 failed.");
    }
#else
    int rc = CCKeyDerivationPBKDF(
        kCCPBKDF2,
        password.data(), password.size(),
        salt, saltLen,
        kCCPRFHmacAlgSHA256,
        static_cast<uint32_t>(iterations),
        derived.data(), 32
    );
    if (rc != kCCSuccess) {
        throw VaultException(VaultErrorKind::CryptoFailure, "CCKeyDerivationPBKDF failed.");
    }
#endif
    return derived;
}

std::array<uint8_t, 32> hkdfSha256(
    const uint8_t* ikm, size_t ikmLen,
    const uint8_t* salt, size_t saltLen,
    const uint8_t* info, size_t infoLen
) {
    // RFC 5869 HKDF-Extract(salt, IKM) -> PRK
    uint8_t zeroSalt[32] = {0};
    const uint8_t* actualSalt = (salt && saltLen > 0) ? salt : zeroSalt;
    size_t actualSaltLen = (salt && saltLen > 0) ? saltLen : 32;
    auto prk = hmacSha256(actualSalt, actualSaltLen, ikm, ikmLen);

    // HKDF-Expand(PRK, info, 32): T(1) = HMAC-SHA256(PRK, info || 0x01)
    std::vector<uint8_t> expandInput(infoLen + 1);
    if (infoLen > 0) std::memcpy(expandInput.data(), info, infoLen);
    expandInput[infoLen] = 0x01;
    auto okm = hmacSha256(prk.data(), prk.size(), expandInput.data(), expandInput.size());
    secureZero(prk.data(), prk.size());
    return okm;
}

std::array<uint8_t, 32> x25519PublicFromPrivate(const std::array<uint8_t, 32>& scalar) {
    static const uint8_t kBasePoint[32] = {9};
    std::array<uint8_t, 32> pub{};
    scalarmult25519(pub.data(), scalar.data(), kBasePoint);
    return pub;
}

std::array<uint8_t, 32> x25519SharedSecret(
    const std::array<uint8_t, 32>& myPrivate,
    const std::array<uint8_t, 32>& peerPublic
) {
    std::array<uint8_t, 32> shared{};
    scalarmult25519(shared.data(), myPrivate.data(), peerPublic.data());
    uint8_t acc = 0;
    for (uint8_t b : shared) acc |= b;
    if (acc == 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "Rejected low-order X25519 public key.");
    }
    return shared;
}

std::vector<uint8_t> aes256GcmSeal(
    const std::array<uint8_t, 32>& key,
    const uint8_t* plaintext, size_t plaintextLen,
    const uint8_t* aad, size_t aadLen,
    const uint8_t* explicitNonce12
) {
    uint8_t nonce[12];
    if (explicitNonce12) {
        std::memcpy(nonce, explicitNonce12, 12);
    } else {
        randomBytes(nonce, 12);
    }

    std::vector<uint8_t> combined(12 + plaintextLen + 16);
    std::memcpy(combined.data(), nonce, 12);
    uint8_t* ctPtr = combined.data() + 12;
    uint8_t* tagPtr = combined.data() + 12 + plaintextLen;

#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "BCryptOpenAlgorithmProvider AES failed.");
    }
    NTSTATUS st = BCryptSetProperty(
        hAlg, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM), 0
    );
    if (st >= 0) {
        st = BCryptGenerateSymmetricKey(
            hAlg, &hKey, nullptr, 0,
            const_cast<PUCHAR>(key.data()), 32, 0
        );
    }
    if (st < 0) {
        if (hKey) BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw VaultException(VaultErrorKind::CryptoFailure, "Failed to configure AES-256-GCM key.");
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = 12;
    authInfo.pbAuthData = (aad && aadLen > 0) ? const_cast<PUCHAR>(aad) : nullptr;
    authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    authInfo.pbTag = tagPtr;
    authInfo.cbTag = 16;

    uint8_t dummyIn = 0, dummyOut = 0;
    ULONG cbResult = 0;
    st = BCryptEncrypt(
        hKey,
        plaintextLen > 0 ? const_cast<PUCHAR>(plaintext) : &dummyIn,
        static_cast<ULONG>(plaintextLen),
        &authInfo,
        nullptr, 0,
        plaintextLen > 0 ? ctPtr : &dummyOut,
        static_cast<ULONG>(plaintextLen),
        &cbResult,
        0
    );
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "BCryptEncrypt AES-256-GCM failed.");
    }
#else
    ref_aes_gcm::Aes256Ctx aes(key.data());
    uint8_t zeroBlk[16] = {0};
    uint8_t H[16] = {0};
    aes.encryptBlock(zeroBlk, H);

    uint8_t J0[16] = {0};
    std::memcpy(J0, nonce, 12);
    J0[15] = 1;

    uint8_t ctr[16];
    std::memcpy(ctr, J0, 16);
    size_t off = 0;
    while (off < plaintextLen) {
        ref_aes_gcm::inc32(ctr);
        uint8_t ks[16];
        aes.encryptBlock(ctr, ks);
        size_t take = std::min<size_t>(16, plaintextLen - off);
        for (size_t i = 0; i < take; ++i) {
            ctPtr[off + i] = plaintext[off + i] ^ ks[i];
        }
        off += take;
    }

    uint8_t S[16], E_J0[16];
    ref_aes_gcm::ghashCompute(H, aad, aadLen, ctPtr, plaintextLen, S);
    aes.encryptBlock(J0, E_J0);
    for (int i = 0; i < 16; ++i) tagPtr[i] = S[i] ^ E_J0[i];
    secureZero(H, sizeof(H));
#endif
    return combined;
}

std::vector<uint8_t> aes256GcmOpen(
    const std::array<uint8_t, 32>& key,
    const uint8_t* combined, size_t combinedLen,
    const uint8_t* aad, size_t aadLen
) {
    if (!combined || combinedLen < 28) {
        throw VaultException(VaultErrorKind::CryptoFailure, "AES-GCM box too short.");
    }
    const uint8_t* nonce = combined;
    size_t ctLen = combinedLen - 28;
    const uint8_t* ctPtr = combined + 12;
    const uint8_t* tagPtr = combined + 12 + ctLen;

    std::vector<uint8_t> plain(ctLen);
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "BCryptOpenAlgorithmProvider AES failed.");
    }
    NTSTATUS st = BCryptSetProperty(
        hAlg, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM), 0
    );
    if (st >= 0) {
        st = BCryptGenerateSymmetricKey(
            hAlg, &hKey, nullptr, 0,
            const_cast<PUCHAR>(key.data()), 32, 0
        );
    }
    if (st < 0) {
        if (hKey) BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw VaultException(VaultErrorKind::CryptoFailure, "Failed to configure AES-256-GCM key.");
    }

    uint8_t tagCopy[16];
    std::memcpy(tagCopy, tagPtr, 16);
    uint8_t nonceCopy[12];
    std::memcpy(nonceCopy, nonce, 12);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonceCopy;
    authInfo.cbNonce = 12;
    authInfo.pbAuthData = (aad && aadLen > 0) ? const_cast<PUCHAR>(aad) : nullptr;
    authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    authInfo.pbTag = tagCopy;
    authInfo.cbTag = 16;

    uint8_t dummyIn = 0, dummyOut = 0;
    ULONG cbResult = 0;
    st = BCryptDecrypt(
        hKey,
        ctLen > 0 ? const_cast<PUCHAR>(ctPtr) : &dummyIn,
        static_cast<ULONG>(ctLen),
        &authInfo,
        nullptr, 0,
        ctLen > 0 ? plain.data() : &dummyOut,
        static_cast<ULONG>(ctLen),
        &cbResult,
        0
    );
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st < 0) {
        secureZeroVec(plain);
        throw VaultException(VaultErrorKind::CryptoFailure, "AES-256-GCM authentication failed.");
    }
#else
    ref_aes_gcm::Aes256Ctx aes(key.data());
    uint8_t zeroBlk[16] = {0};
    uint8_t H[16] = {0};
    aes.encryptBlock(zeroBlk, H);

    uint8_t J0[16] = {0};
    std::memcpy(J0, nonce, 12);
    J0[15] = 1;

    uint8_t S[16], E_J0[16], expectedTag[16];
    ref_aes_gcm::ghashCompute(H, aad, aadLen, ctPtr, ctLen, S);
    aes.encryptBlock(J0, E_J0);
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) {
        expectedTag[i] = S[i] ^ E_J0[i];
        diff |= (expectedTag[i] ^ tagPtr[i]);
    }
    secureZero(H, sizeof(H));
    if (diff != 0) {
        throw VaultException(VaultErrorKind::CryptoFailure, "AES-256-GCM authentication failed.");
    }

    uint8_t ctr[16];
    std::memcpy(ctr, J0, 16);
    size_t off = 0;
    while (off < ctLen) {
        ref_aes_gcm::inc32(ctr);
        uint8_t ks[16];
        aes.encryptBlock(ctr, ks);
        size_t take = std::min<size_t>(16, ctLen - off);
        for (size_t i = 0; i < take; ++i) {
            plain[off + i] = ctPtr[off + i] ^ ks[i];
        }
        off += take;
    }
#endif
    return plain;
}

std::array<uint8_t, 32> deriveClipKey(
    const std::array<uint8_t, 32>& sharedSecret,
    const std::array<uint8_t, 32>& hkdfSalt,
    const UUIDBytes& clipUUID
) {
    size_t pLen = std::strlen(kClipHKDFInfo);
    std::vector<uint8_t> info(pLen + 16);
    std::memcpy(info.data(), kClipHKDFInfo, pLen);
    std::memcpy(info.data() + pLen, clipUUID.bytes.data(), 16);
    return hkdfSha256(sharedSecret.data(), 32, hkdfSalt.data(), 32, info.data(), info.size());
}

std::vector<uint8_t> makeChunkAAD(const UUIDBytes& clipUUID, uint64_t chunkIndex, VaultSegmentKind kind) {
    size_t pLen = std::strlen(kChunkAADPrefix);
    std::vector<uint8_t> aad;
    aad.reserve(pLen + 16 + 8 + 1);
    aad.insert(aad.end(), kChunkAADPrefix, kChunkAADPrefix + pLen);
    aad.insert(aad.end(), clipUUID.bytes.begin(), clipUUID.bytes.end());
    writeBE64(aad, chunkIndex);
    aad.push_back(static_cast<uint8_t>(kind));
    return aad;
}

std::string base64Encode(const uint8_t* data, size_t len) {
    static const char* kTbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTbl[(n >> 18) & 63]);
        out.push_back(kTbl[(n >> 12) & 63]);
        out.push_back((i + 1 < len) ? kTbl[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? kTbl[n & 63] : '=');
    }
    return out;
}

std::vector<uint8_t> base64Decode(const std::string& b64) {
    static const int8_t kDec[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    out.reserve((b64.size() * 3) / 4);
    uint32_t val = 0;
    int valb = -8;
    for (unsigned char c : b64) {
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        int8_t d = kDec[c];
        if (d == -2) break;
        if (d == -1) throw VaultException(VaultErrorKind::CorruptedManifest, "Invalid Base64 character.");
        val = (val << 6) + static_cast<uint32_t>(d);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

} // namespace crypto

// MARK: - VaultClipFileWriter

VaultClipFileWriter::VaultClipFileWriter(
    const std::wstring& path,
    const ClipFileHeader& header,
    const std::array<uint8_t, 32>& clipKey
) : filePath_(path), header_(header), clipKey_(clipKey), hasKey_(true) {
#ifdef _WIN32
    HANDLE h = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        crypto::secureZero(clipKey_.data(), clipKey_.size());
        hasKey_ = false;
        throw VaultException(VaultErrorKind::IoFailure, "Unable to create encrypted vault clip file.");
    }
    fileHandle_ = h;
#else
    std::string u8 = fs::path(path).string();
    int fd = ::open(u8.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        crypto::secureZero(clipKey_.data(), clipKey_.size());
        hasKey_ = false;
        throw VaultException(VaultErrorKind::IoFailure, "Unable to create encrypted vault clip file.");
    }
    fd_ = fd;
#endif

    auto hdrBytes = header_.serialize();
    writeAllAt(hdrBytes.data(), hdrBytes.size(), 0);
    flushDisk();
}

VaultClipFileWriter::~VaultClipFileWriter() {
    if (!isClosed_) {
        try {
            finalizeAndClose(0.0, false);
        } catch (...) {
            closeHandle();
        }
    }
    if (hasKey_) {
        crypto::secureZero(clipKey_.data(), clipKey_.size());
        hasKey_ = false;
    }
}

int VaultClipFileWriter::writtenChunkCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(nextChunkIndex_);
}

void VaultClipFileWriter::appendSegment(const uint8_t* plaintext, size_t len, VaultSegmentKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isClosed_ || !hasKey_) {
        throw VaultException(VaultErrorKind::IoFailure, "Vault clip writer is already closed.");
    }
    if (!plaintext || len == 0) return;

    uint64_t idx = nextChunkIndex_;
    auto aad = crypto::makeChunkAAD(header_.clipUUID, idx, kind);
    auto combined = crypto::aes256GcmSeal(clipKey_, plaintext, len, aad.data(), aad.size());
    if (combined.size() > 64000000) {
        throw VaultException(VaultErrorKind::CryptoFailure, "Encrypted segment exceeds 64 MB safety bound.");
    }

    std::vector<uint8_t> packet;
    packet.reserve(5 + combined.size());
    writeBE32(packet, static_cast<uint32_t>(combined.size()));
    packet.push_back(static_cast<uint8_t>(kind));
    packet.insert(packet.end(), combined.begin(), combined.end());

    writeAllAtEnd(packet.data(), packet.size());
    flushDisk();
    nextChunkIndex_++;
}

void VaultClipFileWriter::finalizeAndClose(double durationSeconds, bool cleanlyFinalized) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isClosed_) return;
    isClosed_ = true;
    if (hasKey_) {
        crypto::secureZero(clipKey_.data(), clipKey_.size());
        hasKey_ = false;
    }

    if (cleanlyFinalized) {
        header_.flags |= 0x02;
    }
    if (durationSeconds > 0.0) {
        header_.durationMs = static_cast<uint64_t>(durationSeconds * 1000.0 + 0.5);
    }
    try {
        auto updated = header_.serialize();
        writeAllAt(updated.data(), updated.size(), 0);
        flushDisk();
    } catch (...) {
        closeHandle();
        throw;
    }
    closeHandle();
}

void VaultClipFileWriter::writeAllAt(const uint8_t* data, size_t len, uint64_t offset) {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(fileHandle_);
    if (h == INVALID_HANDLE_VALUE) throw VaultException(VaultErrorKind::IoFailure, "Invalid file handle.");
    size_t writtenTotal = 0;
    while (writtenTotal < len) {
        OVERLAPPED ov{};
        uint64_t cur = offset + writtenTotal;
        ov.Offset = static_cast<DWORD>(cur & 0xFFFFFFFFULL);
        ov.OffsetHigh = static_cast<DWORD>(cur >> 32);
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - writtenTotal, 16 * 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(h, data + writtenTotal, chunk, &written, &ov)) {
            throw VaultException(VaultErrorKind::IoFailure, "WriteFile failed on vault clip.");
        }
        writtenTotal += written;
    }
#else
    size_t writtenTotal = 0;
    while (writtenTotal < len) {
        ssize_t n = ::pwrite(fd_, data + writtenTotal, len - writtenTotal, static_cast<off_t>(offset + writtenTotal));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw VaultException(VaultErrorKind::IoFailure, "pwrite failed.");
        }
        writtenTotal += static_cast<size_t>(n);
    }
#endif
}

void VaultClipFileWriter::writeAllAtEnd(const uint8_t* data, size_t len) {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(fileHandle_);
    LARGE_INTEGER sizeLi{};
    if (!GetFileSizeEx(h, &sizeLi)) {
        throw VaultException(VaultErrorKind::IoFailure, "GetFileSizeEx failed.");
    }
    writeAllAt(data, len, static_cast<uint64_t>(sizeLi.QuadPart));
#else
    off_t endPos = ::lseek(fd_, 0, SEEK_END);
    if (endPos < 0) throw VaultException(VaultErrorKind::IoFailure, "lseek failed.");
    writeAllAt(data, len, static_cast<uint64_t>(endPos));
#endif
}

void VaultClipFileWriter::flushDisk() {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(fileHandle_);
    if (h != INVALID_HANDLE_VALUE) {
        if (!FlushFileBuffers(h)) {
            throw VaultException(VaultErrorKind::IoFailure, "FlushFileBuffers failed.");
        }
    }
#else
    if (fd_ >= 0) {
#ifdef F_FULLFSYNC
        if (::fcntl(fd_, F_FULLFSYNC) != 0) ::fsync(fd_);
#else
        ::fsync(fd_);
#endif
    }
#endif
}

void VaultClipFileWriter::closeHandle() {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(fileHandle_);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        fileHandle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

// MARK: - VaultClipReader

VaultClipReader::VaultClipReader(const std::wstring& filePath, const std::array<uint8_t, 32>& clipKey)
    : filePath_(filePath), clipKey_(clipKey) {
    uint64_t fileSize = 0;
#ifdef _WIN32
    HANDLE h = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        crypto::secureZero(clipKey_.data(), clipKey_.size());
        throw VaultException(VaultErrorKind::IoFailure, "Cannot open encrypted clip.");
    }
    fileHandle_ = h;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart < static_cast<LONGLONG>(ClipFileHeader::kByteSize)) {
        CloseHandle(h);
        fileHandle_ = INVALID_HANDLE_VALUE;
        crypto::secureZero(clipKey_.data(), clipKey_.size());
        throw VaultException(VaultErrorKind::CorruptedClipHeader, "Clip file smaller than MWV1 header.");
    }
    fileSize = static_cast<uint64_t>(li.QuadPart);
#else
    std::string u8 = fs::path(filePath).string();
    int fd = ::open(u8.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        crypto::secureZero(clipKey_.data(), clipKey_.size());
        throw VaultException(VaultErrorKind::IoFailure, "Cannot open encrypted clip.");
    }
    fd_ = fd;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(ClipFileHeader::kByteSize)) {
        ::close(fd);
        fd_ = -1;
        crypto::secureZero(clipKey_.data(), clipKey_.size());
        throw VaultException(VaultErrorKind::CorruptedClipHeader, "Clip file smaller than MWV1 header.");
    }
    fileSize = static_cast<uint64_t>(st.st_size);
#endif

    try {
#ifdef _WIN32
        auto hdrData = preadExactStatic(fileHandle_, 0, ClipFileHeader::kByteSize);
#else
        auto hdrData = preadExactStatic(fd_, 0, ClipFileHeader::kByteSize);
#endif
        header_ = ClipFileHeader::parse(hdrData.data(), hdrData.size());

        uint64_t cursor = ClipFileHeader::kByteSize;
        int64_t virtualPos = 0;
        uint64_t idx = 0;

        while (cursor + 5 <= fileSize) {
            std::vector<uint8_t> pref;
            try {
#ifdef _WIN32
                pref = preadExactStatic(fileHandle_, cursor, 5);
#else
                pref = preadExactStatic(fd_, cursor, 5);
#endif
            } catch (...) {
                break;
            }
            if (pref.size() != 5) break;
            uint32_t cLen = readBE32(pref.data());
            uint8_t rawKind = pref[4];
            if (cLen < 28 || cLen > 64000000 ||
                (rawKind != 1 && rawKind != 2) ||
                cursor + 5 + static_cast<uint64_t>(cLen) > fileSize) {
                break;
            }
            size_t plainLen = static_cast<size_t>(cLen) - 28;
            IndexedEncryptedChunk entry;
            entry.chunkIndex = idx;
            entry.kind = static_cast<VaultSegmentKind>(rawKind);
            entry.filePayloadOffset = cursor + 5;
            entry.combinedLength = cLen;
            entry.plaintextOffset = virtualPos;
            entry.plaintextLength = plainLen;
            chunks_.push_back(entry);
            virtualPos += static_cast<int64_t>(plainLen);
            cursor += 5 + static_cast<uint64_t>(cLen);
            idx++;
        }

        // Strip any half-written power-loss tail chunk whose GCM tag fails
        while (!chunks_.empty()) {
            try {
#ifdef _WIN32
                auto dec = decryptChunkStatic(fileHandle_, chunks_.back(), header_.clipUUID, clipKey_);
#else
                auto dec = decryptChunkStatic(fd_, chunks_.back(), header_.clipUUID, clipKey_);
#endif
                crypto::secureZeroVec(dec);
                break;
            } catch (...) {
                virtualPos -= static_cast<int64_t>(chunks_.back().plaintextLength);
                chunks_.pop_back();
            }
        }

        if (!chunks_.empty()) {
#ifdef _WIN32
            auto dec0 = decryptChunkStatic(fileHandle_, chunks_.front(), header_.clipUUID, clipKey_);
#else
            auto dec0 = decryptChunkStatic(fd_, chunks_.front(), header_.clipUUID, clipKey_);
#endif
            crypto::secureZeroVec(dec0);
        }

        totalPlaintextLength_ = virtualPos;
        mediaFragmentCount_ = 0;
        for (const auto& c : chunks_) {
            if (c.kind == VaultSegmentKind::MediaFragment) mediaFragmentCount_++;
        }
    } catch (...) {
#ifdef _WIN32
        if (fileHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(fileHandle_));
            fileHandle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
        crypto::secureZero(clipKey_.data(), clipKey_.size());
        throw;
    }
}

VaultClipReader::~VaultClipReader() {
#ifdef _WIN32
    if (fileHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(fileHandle_));
        fileHandle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    crypto::secureZero(clipKey_.data(), clipKey_.size());
    std::lock_guard<std::mutex> lock(cacheMutex_);
    for (auto& kv : decryptedCache_) {
        crypto::secureZeroVec(kv.second);
    }
    decryptedCache_.clear();
    cacheOrder_.clear();
}

std::vector<uint8_t> VaultClipReader::readPlaintextRange(int64_t offset, size_t length) {
    if (length == 0 || offset < 0 || offset >= totalPlaintextLength_ || chunks_.empty()) {
        return {};
    }
    int64_t clampedEnd = std::min<int64_t>(totalPlaintextLength_, offset + static_cast<int64_t>(length));
    size_t targetLen = static_cast<size_t>(clampedEnd - offset);
    std::vector<uint8_t> out;
    out.reserve(targetLen);

    // Binary search first chunk whose end > offset
    size_t lo = 0, hi = chunks_.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const auto& c = chunks_[mid];
        if (c.plaintextOffset + static_cast<int64_t>(c.plaintextLength) > offset) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    for (size_t idx = lo; idx < chunks_.size(); ++idx) {
        const auto& c = chunks_[idx];
        if (c.plaintextOffset >= clampedEnd) break;
        auto plain = decryptedChunk(c);
        size_t copyStart = static_cast<size_t>(std::max<int64_t>(0, offset - c.plaintextOffset));
        size_t copyEnd = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(c.plaintextLength), clampedEnd - c.plaintextOffset));
        if (copyEnd > copyStart) {
            out.insert(out.end(), plain.begin() + copyStart, plain.begin() + copyEnd);
        }
        crypto::secureZeroVec(plain);
    }
    return out;
}

std::vector<uint8_t> VaultClipReader::decryptedChunk(const IndexedEncryptedChunk& chunk) {
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = decryptedCache_.find(chunk.chunkIndex);
        if (it != decryptedCache_.end()) {
            auto pos = std::find(cacheOrder_.begin(), cacheOrder_.end(), chunk.chunkIndex);
            if (pos != cacheOrder_.end()) cacheOrder_.erase(pos);
            cacheOrder_.push_back(chunk.chunkIndex);
            return it->second;
        }
    }

#ifdef _WIN32
    auto dec = decryptChunkStatic(fileHandle_, chunk, header_.clipUUID, clipKey_);
#else
    auto dec = decryptChunkStatic(fd_, chunk, header_.clipUUID, clipKey_);
#endif

    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        decryptedCache_[chunk.chunkIndex] = dec;
        cacheOrder_.push_back(chunk.chunkIndex);
        while (cacheOrder_.size() > kMaxCachedChunks) {
            uint64_t evict = cacheOrder_.front();
            cacheOrder_.erase(cacheOrder_.begin());
            auto oldIt = decryptedCache_.find(evict);
            if (oldIt != decryptedCache_.end()) {
                crypto::secureZeroVec(oldIt->second);
                decryptedCache_.erase(oldIt);
            }
        }
    }
    return dec;
}

std::vector<uint8_t> VaultClipReader::decryptChunkStatic(
#ifdef _WIN32
    void* handle,
#else
    int fd,
#endif
    const IndexedEncryptedChunk& chunk,
    const UUIDBytes& clipUUID,
    const std::array<uint8_t, 32>& key
) {
#ifdef _WIN32
    auto combined = preadExactStatic(handle, chunk.filePayloadOffset, chunk.combinedLength);
#else
    auto combined = preadExactStatic(fd, chunk.filePayloadOffset, chunk.combinedLength);
#endif
    auto aad = crypto::makeChunkAAD(clipUUID, chunk.chunkIndex, chunk.kind);
    return crypto::aes256GcmOpen(key, combined.data(), combined.size(), aad.data(), aad.size());
}

std::vector<uint8_t> VaultClipReader::preadExactStatic(
#ifdef _WIN32
    void* handle,
#else
    int fd,
#endif
    uint64_t offset,
    size_t length
) {
    std::vector<uint8_t> buf(length);
    size_t totalRead = 0;
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(handle);
    while (totalRead < length) {
        OVERLAPPED ov{};
        uint64_t cur = offset + totalRead;
        ov.Offset = static_cast<DWORD>(cur & 0xFFFFFFFFULL);
        ov.OffsetHigh = static_cast<DWORD>(cur >> 32);
        DWORD want = static_cast<DWORD>(std::min<size_t>(length - totalRead, 16 * 1024 * 1024));
        DWORD got = 0;
        if (!ReadFile(h, buf.data() + totalRead, want, &got, &ov) || got == 0) {
            throw VaultException(VaultErrorKind::IoFailure, "Unexpected EOF reading vault clip.");
        }
        totalRead += got;
    }
#else
    while (totalRead < length) {
        ssize_t n = ::pread(fd, buf.data() + totalRead, length - totalRead, static_cast<off_t>(offset + totalRead));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw VaultException(VaultErrorKind::IoFailure, "pread failed.");
        }
        if (n == 0) {
            throw VaultException(VaultErrorKind::IoFailure, "Unexpected EOF reading vault clip.");
        }
        totalRead += static_cast<size_t>(n);
    }
#endif
    return buf;
}

// MARK: - VaultManager

VaultManager::VaultManager(const std::wstring& customVaultDir) {
    if (!customVaultDir.empty()) {
        vaultDir_ = customVaultDir;
    } else {
#ifdef _WIN32
        PWSTR localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)) && localAppData) {
            fs::path base(localAppData);
            CoTaskMemFree(localAppData);
            vaultDir_ = (base / L"MaxWatch" / L"Vault").wstring();
        } else {
            vaultDir_ = (fs::current_path() / L"MaxWatchVault").wstring();
        }
#else
        const char* home = std::getenv("HOME");
        fs::path base = home ? fs::path(home) : fs::current_path();
        vaultDir_ = (base / "Library" / "Application Support" / "MaxWatch" / "Vault").wstring();
#endif
    }
    clipsDir_ = (fs::path(vaultDir_) / L"Clips").wstring();
    manifestPath_ = (fs::path(vaultDir_) / L"vault_manifest.json").wstring();
    ensureDirectories();
}

VaultManager::~VaultManager() {
    lock();
}

void VaultManager::ensureDirectories() const {
    std::error_code ec;
    fs::create_directories(fs::path(vaultDir_), ec);
    fs::create_directories(fs::path(clipsDir_), ec);
#ifdef _WIN32
    // Harden NTFS ACL on Vault directory so only SYSTEM, Administrators, and the Owner/Current User have access
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD len = 0;
        GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
        if (len > 0) {
            std::vector<uint8_t> tokBuf(len);
            if (GetTokenInformation(hToken, TokenUser, tokBuf.data(), len, &len)) {
                auto* tu = reinterpret_cast<TOKEN_USER*>(tokBuf.data());
                LPWSTR sidStr = nullptr;
                if (ConvertSidToStringSidW(tu->User.Sid, &sidStr) && sidStr) {
                    std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + std::wstring(sidStr) + L")";
                    LocalFree(sidStr);
                    PSECURITY_DESCRIPTOR pSD = nullptr;
                    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &pSD, nullptr)) {
                        PACL pDacl = nullptr;
                        BOOL daclPresent = FALSE, daclDefaulted = FALSE;
                        if (GetSecurityDescriptorDacl(pSD, &daclPresent, &pDacl, &daclDefaulted) && daclPresent) {
                            SetNamedSecurityInfoW(
                                const_cast<LPWSTR>(vaultDir_.c_str()),
                                SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, pDacl, nullptr
                            );
                        }
                        LocalFree(pSD);
                    }
                }
            }
        }
        CloseHandle(hToken);
    }
#else
    ::chmod(fs::path(vaultDir_).string().c_str(), 0700);
    ::chmod(fs::path(clipsDir_).string().c_str(), 0700);
#endif
}

bool VaultManager::isInitialized() const {
    std::error_code ec;
    return fs::exists(fs::path(manifestPath_), ec);
}

bool VaultManager::isUnlocked() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return unlockedPrivateKey_.size() == 32;
}

void VaultManager::initializeVault(const std::string& password) {
    if (password.size() < 4) {
        throw VaultException(VaultErrorKind::PasswordTooShort, "Password must be at least 4 characters.");
    }
    ensureDirectories();
    if (isInitialized()) {
        throw VaultException(VaultErrorKind::VaultAlreadyInitialized, "Vault is already initialized.");
    }

    auto salt = crypto::randomBytesVec(32);
    auto kek = crypto::derivePBKDF2Sha256(password, salt.data(), salt.size(), kPbkdf2Iterations);

    std::array<uint8_t, 32> privRaw{};
    crypto::randomBytes(privRaw.data(), 32);
    auto pubRaw = crypto::x25519PublicFromPrivate(privRaw);

    auto wrapped = crypto::aes256GcmSeal(
        kek,
        privRaw.data(), privRaw.size(),
        reinterpret_cast<const uint8_t*>(kWrapAAD), std::strlen(kWrapAAD)
    );
    crypto::secureZero(privRaw.data(), privRaw.size());
    crypto::secureZero(kek.data(), kek.size());

    VaultManifest m;
    m.version = 1;
    m.pbkdf2Iterations = kPbkdf2Iterations;
    m.pbkdf2SaltBase64 = crypto::base64Encode(salt.data(), salt.size());
    m.publicKeyBase64 = crypto::base64Encode(pubRaw.data(), pubRaw.size());
    m.wrappedPrivateKeyBase64 = crypto::base64Encode(wrapped.data(), wrapped.size());
    m.createdAtISO8601 = currentISO8601UTC();

    writeManifestAtomically(m);

    // Security invariant: Vault starts strictly LOCKED immediately after password creation!
    lock();
}

bool VaultManager::unlock(const std::string& password) {
    VaultManifest m = loadManifest();
    auto salt = crypto::base64Decode(m.pbkdf2SaltBase64);
    auto wrapped = crypto::base64Decode(m.wrappedPrivateKeyBase64);
    auto expectedPub = crypto::base64Decode(m.publicKeyBase64);
    if (salt.size() != 32 || expectedPub.size() != 32 || wrapped.size() != 60) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Vault manifest key sizes are invalid.");
    }

    auto kek = crypto::derivePBKDF2Sha256(password, salt.data(), salt.size(), m.pbkdf2Iterations);
    std::vector<uint8_t> privVec;
    try {
        privVec = crypto::aes256GcmOpen(
            kek,
            wrapped.data(), wrapped.size(),
            reinterpret_cast<const uint8_t*>(kWrapAAD), std::strlen(kWrapAAD)
        );
    } catch (...) {
        crypto::secureZero(kek.data(), kek.size());
        return false;
    }
    crypto::secureZero(kek.data(), kek.size());

    if (privVec.size() != 32) {
        crypto::secureZeroVec(privVec);
        throw VaultException(VaultErrorKind::CorruptedManifest, "Unwrapped private key length invalid.");
    }
    std::array<uint8_t, 32> privArr{};
    std::memcpy(privArr.data(), privVec.data(), 32);
    auto derivedPub = crypto::x25519PublicFromPrivate(privArr);
    crypto::secureZero(privArr.data(), 32);
    if (std::memcmp(derivedPub.data(), expectedPub.data(), 32) != 0) {
        crypto::secureZeroVec(privVec);
        throw VaultException(VaultErrorKind::CorruptedManifest, "Vault public/private keypair mismatch.");
    }

    std::lock_guard<std::mutex> guard(stateMutex_);
    if (!unlockedPrivateKey_.empty()) {
#ifdef _WIN32
        if (privateKeyMemoryLocked_) {
            VirtualUnlock(unlockedPrivateKey_.data(), unlockedPrivateKey_.size());
            privateKeyMemoryLocked_ = false;
        }
#endif
        crypto::secureZeroVec(unlockedPrivateKey_);
    }
    unlockedPrivateKey_ = std::move(privVec);
#ifdef _WIN32
    // Prevent Windows from ever swapping the unlocked 32-byte key out to pagefile.sys
    privateKeyMemoryLocked_ = (VirtualLock(unlockedPrivateKey_.data(), unlockedPrivateKey_.size()) != 0);
#endif
    return true;
}

void VaultManager::lock() {
    std::lock_guard<std::mutex> guard(stateMutex_);
    if (!unlockedPrivateKey_.empty()) {
#ifdef _WIN32
        if (privateKeyMemoryLocked_) {
            crypto::secureZero(unlockedPrivateKey_.data(), unlockedPrivateKey_.size());
            VirtualUnlock(unlockedPrivateKey_.data(), unlockedPrivateKey_.size());
            privateKeyMemoryLocked_ = false;
        }
#endif
        crypto::secureZeroVec(unlockedPrivateKey_);
    }
}

void VaultManager::changePassword(const std::string& currentPassword, const std::string& newPassword) {
    if (newPassword.size() < 4) {
        throw VaultException(VaultErrorKind::PasswordTooShort, "Password must be at least 4 characters.");
    }
    VaultManifest m = loadManifest();
    auto oldSalt = crypto::base64Decode(m.pbkdf2SaltBase64);
    auto oldWrapped = crypto::base64Decode(m.wrappedPrivateKeyBase64);

    auto oldKek = crypto::derivePBKDF2Sha256(currentPassword, oldSalt.data(), oldSalt.size(), m.pbkdf2Iterations);
    std::vector<uint8_t> privVec;
    try {
        privVec = crypto::aes256GcmOpen(
            oldKek,
            oldWrapped.data(), oldWrapped.size(),
            reinterpret_cast<const uint8_t*>(kWrapAAD), std::strlen(kWrapAAD)
        );
    } catch (...) {
        crypto::secureZero(oldKek.data(), oldKek.size());
        throw VaultException(VaultErrorKind::InvalidPassword, "Incorrect current vault password.");
    }
    crypto::secureZero(oldKek.data(), oldKek.size());

    auto newSalt = crypto::randomBytesVec(32);
    auto newKek = crypto::derivePBKDF2Sha256(newPassword, newSalt.data(), newSalt.size(), kPbkdf2Iterations);
    auto newWrapped = crypto::aes256GcmSeal(
        newKek,
        privVec.data(), privVec.size(),
        reinterpret_cast<const uint8_t*>(kWrapAAD), std::strlen(kWrapAAD)
    );
    crypto::secureZeroVec(privVec);
    crypto::secureZero(newKek.data(), newKek.size());

    VaultManifest updated = m;
    updated.pbkdf2Iterations = kPbkdf2Iterations;
    updated.pbkdf2SaltBase64 = crypto::base64Encode(newSalt.data(), newSalt.size());
    updated.wrappedPrivateKeyBase64 = crypto::base64Encode(newWrapped.data(), newWrapped.size());
    writeManifestAtomically(updated);
}

void VaultManager::resetVaultDeletingAllClips() {
    lock();
    std::error_code ec;
    if (fs::exists(fs::path(clipsDir_), ec)) {
        for (const auto& entry : fs::directory_iterator(fs::path(clipsDir_), ec)) {
            if (entry.is_regular_file()) {
                try { secureDeleteClip(entry.path().wstring()); } catch (...) {}
            }
        }
        fs::remove_all(fs::path(clipsDir_), ec);
    }
    if (fs::exists(fs::path(manifestPath_), ec)) {
        fs::remove(fs::path(manifestPath_), ec);
    }
    ensureDirectories();
}

std::unique_ptr<VaultClipFileWriter> VaultManager::createClipWriter(int width, int height, bool hasAudio) {
    ensureDirectories();
    VaultManifest m = loadManifest();
    auto pubVec = crypto::base64Decode(m.publicKeyBase64);
    if (pubVec.size() != 32) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Invalid vault public key.");
    }
    std::array<uint8_t, 32> vaultPub{};
    std::memcpy(vaultPub.data(), pubVec.data(), 32);

    std::array<uint8_t, 32> ephPriv{};
    crypto::randomBytes(ephPriv.data(), 32);
    auto ephPub = crypto::x25519PublicFromPrivate(ephPriv);
    auto shared = crypto::x25519SharedSecret(ephPriv, vaultPub);
    crypto::secureZero(ephPriv.data(), 32);

    std::array<uint8_t, 32> hkdfSalt{};
    crypto::randomBytes(hkdfSalt.data(), 32);
    UUIDBytes uuid = UUIDBytes::generateRandom();
    auto clipKey = crypto::deriveClipKey(shared, hkdfSalt, uuid);
    crypto::secureZero(shared.data(), 32);

    auto now = std::chrono::system_clock::now();
    int64_t unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm loc{};
#ifdef _WIN32
    localtime_s(&loc, &tt);
#else
    localtime_r(&tt, &loc);
#endif
    char stampBuf[32];
    std::strftime(stampBuf, sizeof(stampBuf), "%Y-%m-%d_%H-%M-%S", &loc);

    std::string fname = std::string("MaxWatch_") + stampBuf + "_" + uuid.shortPrefix8() + ".mwvault";
    std::wstring fullPath = (fs::path(clipsDir_) / fname).wstring();

    ClipFileHeader hdr;
    hdr.version = ClipFileHeader::kCurrentVersion;
    hdr.flags = hasAudio ? 0x01 : 0x00;
    hdr.clipUUID = uuid;
    hdr.createdUnixMs = unixMs;
    hdr.width = static_cast<uint32_t>(std::max(1, width));
    hdr.height = static_cast<uint32_t>(std::max(1, height));
    hdr.ephemeralPublicKeyRaw = ephPub;
    hdr.hkdfSalt = hkdfSalt;
    hdr.durationMs = 0;

    auto writer = std::make_unique<VaultClipFileWriter>(fullPath, hdr, clipKey);
    crypto::secureZero(clipKey.data(), 32);
    return writer;
}

std::unique_ptr<VaultClipReader> VaultManager::openClipReader(const std::wstring& clipPath) {
    auto privKey = copyUnlockedPrivateKey();

#ifdef _WIN32
    HANDLE h = CreateFileW(
        clipPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        crypto::secureZero(privKey.data(), 32);
        throw VaultException(VaultErrorKind::IoFailure, "Unable to open clip file.");
    }
    std::vector<uint8_t> hdrData;
    try {
        hdrData = VaultClipReader::preadExactStatic(h, 0, ClipFileHeader::kByteSize);
        CloseHandle(h);
    } catch (...) {
        CloseHandle(h);
        crypto::secureZero(privKey.data(), 32);
        throw;
    }
#else
    std::string u8 = fs::path(clipPath).string();
    int fd = ::open(u8.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        crypto::secureZero(privKey.data(), 32);
        throw VaultException(VaultErrorKind::IoFailure, "Unable to open clip file.");
    }
    std::vector<uint8_t> hdrData;
    try {
        hdrData = VaultClipReader::preadExactStatic(fd, 0, ClipFileHeader::kByteSize);
        ::close(fd);
    } catch (...) {
        ::close(fd);
        crypto::secureZero(privKey.data(), 32);
        throw;
    }
#endif

    ClipFileHeader hdr = ClipFileHeader::parse(hdrData.data(), hdrData.size());
    auto shared = crypto::x25519SharedSecret(privKey, hdr.ephemeralPublicKeyRaw);
    crypto::secureZero(privKey.data(), 32);

    auto clipKey = crypto::deriveClipKey(shared, hdr.hkdfSalt, hdr.clipUUID);
    crypto::secureZero(shared.data(), 32);

    try {
        auto reader = std::make_unique<VaultClipReader>(clipPath, clipKey);
        crypto::secureZero(clipKey.data(), 32);
        return reader;
    } catch (...) {
        crypto::secureZero(clipKey.data(), 32);
        throw;
    }
}

int VaultManager::countVaultClips() const {
    std::error_code ec;
    if (!fs::exists(fs::path(clipsDir_), ec)) return 0;
    int count = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(clipsDir_), ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".mwvault") count++;
    }
    return count;
}

std::vector<EncryptedClipSummary> VaultManager::listSummaries(const std::wstring& excludeActivePath) const {
    std::vector<EncryptedClipSummary> results;
    std::error_code ec;
    if (!fs::exists(fs::path(clipsDir_), ec)) return results;

    fs::path exclNorm;
    if (!excludeActivePath.empty()) {
        exclNorm = fs::weakly_canonical(fs::path(excludeActivePath), ec);
    }

    for (const auto& entry : fs::directory_iterator(fs::path(clipsDir_), ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext != L".mwvault") continue;

        if (!excludeActivePath.empty()) {
            auto curNorm = fs::weakly_canonical(entry.path(), ec);
            if (curNorm == exclNorm) continue;
        }

        uint64_t fSize = entry.file_size(ec);
        if (ec || fSize < ClipFileHeader::kByteSize) continue;

#ifdef _WIN32
        HANDLE h = CreateFileW(
            entry.path().c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (h == INVALID_HANDLE_VALUE) continue;
#else
        int fd = ::open(entry.path().string().c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
#endif

        ClipFileHeader hdr;
        int chunkCount = 0;
        int mediaFragments = 0;
        bool headerOk = false;
        try {
#ifdef _WIN32
            auto hdrBytes = VaultClipReader::preadExactStatic(h, 0, ClipFileHeader::kByteSize);
#else
            auto hdrBytes = VaultClipReader::preadExactStatic(fd, 0, ClipFileHeader::kByteSize);
#endif
            hdr = ClipFileHeader::parse(hdrBytes.data(), hdrBytes.size());
            headerOk = true;

            uint64_t cursor = ClipFileHeader::kByteSize;
            while (cursor + 5 <= fSize) {
#ifdef _WIN32
                auto pref = VaultClipReader::preadExactStatic(h, cursor, 5);
#else
                auto pref = VaultClipReader::preadExactStatic(fd, cursor, 5);
#endif
                uint32_t cLen = readBE32(pref.data());
                uint8_t rawKind = pref[4];
                if (cLen < 28 || cLen > 64000000 || (rawKind != 1 && rawKind != 2) ||
                    cursor + 5 + static_cast<uint64_t>(cLen) > fSize) {
                    break;
                }
                chunkCount++;
                if (rawKind == 2) mediaFragments++;
                cursor += 5 + static_cast<uint64_t>(cLen);
            }
        } catch (...) {
        }

#ifdef _WIN32
        CloseHandle(h);
#else
        ::close(fd);
#endif

        if (!headerOk || chunkCount < 2 || mediaFragments < 1) continue;

        double hdrDur = static_cast<double>(hdr.durationMs) / 1000.0;
        double estDur = (hdrDur > 0.1) ? hdrDur : static_cast<double>(mediaFragments) * 1.0;

        EncryptedClipSummary s;
        s.filePath = entry.path().wstring();
        s.fileName = entry.path().filename().wstring();
        s.clipUUID = hdr.clipUUID;
        s.createdUnixMs = hdr.createdUnixMs;
        s.fileSizeBytes = static_cast<int64_t>(fSize);
        s.width = static_cast<int>(hdr.width);
        s.height = static_cast<int>(hdr.height);
        s.hasAudio = hdr.hasAudio();
        s.cleanlyFinalized = hdr.cleanlyFinalized();
        s.headerDurationSeconds = hdrDur;
        s.validChunkCount = chunkCount;
        s.estimatedDurationSeconds = estDur;
        results.push_back(std::move(s));
    }

    std::sort(results.begin(), results.end(), [](const EncryptedClipSummary& a, const EncryptedClipSummary& b) {
        return a.createdUnixMs < b.createdUnixMs;
    });
    return results;
}

void VaultManager::secureDeleteClip(const std::wstring& clipPath) {
    std::error_code ec;
    uint64_t fSize = fs::file_size(fs::path(clipPath), ec);
    if (!ec && fSize > 0) {
        size_t wipeBytes = static_cast<size_t>(std::min<uint64_t>(fSize, 131072));
        std::vector<uint8_t> zeros(wipeBytes, 0);
#ifdef _WIN32
        HANDLE h = CreateFileW(
            clipPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, zeros.data(), static_cast<DWORD>(wipeBytes), &written, nullptr);
            FlushFileBuffers(h);
            CloseHandle(h);
        }
#else
        int fd = ::open(fs::path(clipPath).string().c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            (void)::pwrite(fd, zeros.data(), wipeBytes, 0);
            ::fsync(fd);
            ::close(fd);
        }
#endif
    }
    if (!fs::remove(fs::path(clipPath), ec)) {
        throw VaultException(VaultErrorKind::IoFailure, "Failed to delete vault clip.");
    }
}

std::wstring VaultManager::exportClipToDirectory(const std::wstring& clipPath, const std::wstring& directoryPath) {
    if (isPathInsideVault(directoryPath)) {
        throw VaultException(VaultErrorKind::IoFailure, "Please choose a folder outside the internal Encrypted Vault.");
    }
    std::error_code ec;
    fs::create_directories(fs::path(directoryPath), ec);

    std::wstring stem = fs::path(clipPath).stem().wstring();
    fs::path candidate = fs::path(directoryPath) / (stem + L".mp4");
    int suffix = 2;
    while (fs::exists(candidate, ec)) {
        candidate = fs::path(directoryPath) / (stem + L"-" + std::to_wstring(suffix) + L".mp4");
        suffix++;
    }
    exportClipAsMP4(clipPath, candidate.wstring());
    return candidate.wstring();
}

void VaultManager::exportClipAsMP4(const std::wstring& clipPath, const std::wstring& destinationPath) {
    if (isPathInsideVault(destinationPath)) {
        throw VaultException(VaultErrorKind::IoFailure, "Cannot export plaintext MP4 inside the internal Encrypted Vault.");
    }
    auto reader = openClipReader(clipPath);
#ifdef _WIN32
    HANDLE hOut = CreateFileW(
        destinationPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hOut == INVALID_HANDLE_VALUE) {
        throw VaultException(VaultErrorKind::IoFailure, "Cannot create destination MP4 file.");
    }
    int64_t offset = 0;
    constexpr size_t kStep = 1048576;
    try {
        while (offset < reader->totalPlaintextLength()) {
            auto chunk = reader->readPlaintextRange(offset, kStep);
            if (chunk.empty()) break;
            DWORD written = 0;
            BOOL ok = WriteFile(hOut, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr);
            size_t n = chunk.size();
            crypto::secureZeroVec(chunk);
            if (!ok || written != n) {
                throw VaultException(VaultErrorKind::IoFailure, "MP4 export write failed.");
            }
            offset += static_cast<int64_t>(n);
        }
        FlushFileBuffers(hOut);
        CloseHandle(hOut);
    } catch (...) {
        CloseHandle(hOut);
        throw;
    }
#else
    std::ofstream ofs(fs::path(destinationPath), std::ios::binary | std::ios::trunc);
    if (!ofs) throw VaultException(VaultErrorKind::IoFailure, "Cannot create destination MP4 file.");
    int64_t offset = 0;
    constexpr size_t kStep = 1048576;
    while (offset < reader->totalPlaintextLength()) {
        auto chunk = reader->readPlaintextRange(offset, kStep);
        if (chunk.empty()) break;
        ofs.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        offset += static_cast<int64_t>(chunk.size());
        crypto::secureZeroVec(chunk);
    }
    ofs.flush();
#endif
}

bool VaultManager::isPathInsideVault(const std::wstring& candidatePath) const {
    std::error_code ec;
    auto cand = fs::weakly_canonical(fs::path(candidatePath), ec).wstring();
    auto vdir = fs::weakly_canonical(fs::path(vaultDir_), ec).wstring();
#ifdef _WIN32
    std::transform(cand.begin(), cand.end(), cand.begin(), ::towlower);
    std::transform(vdir.begin(), vdir.end(), vdir.begin(), ::towlower);
    const wchar_t sep = L'\\';
#else
    const wchar_t sep = L'/';
#endif
    while (!vdir.empty() && (vdir.back() == L'\\' || vdir.back() == L'/')) vdir.pop_back();
    if (cand == vdir) return true;
    if (cand.size() > vdir.size() && cand.compare(0, vdir.size(), vdir) == 0 &&
        (cand[vdir.size()] == sep || cand[vdir.size()] == L'/')) {
        return true;
    }
    return false;
}

VaultManifest VaultManager::loadManifest() const {
    if (!isInitialized()) {
        throw VaultException(VaultErrorKind::VaultNotInitialized, "Vault password has not been set yet.");
    }
    std::ifstream ifs(fs::path(manifestPath_), std::ios::binary);
    if (!ifs) {
        throw VaultException(VaultErrorKind::CorruptedManifest, "Unable to read vault_manifest.json.");
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return VaultManifest::fromJson(content);
}

void VaultManager::writeManifestAtomically(const VaultManifest& manifest) const {
    std::string json = manifest.toJson();
    fs::path tmpPath = fs::path(vaultDir_) / L"vault_manifest.tmp";
    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            throw VaultException(VaultErrorKind::IoFailure, "Cannot write temporary vault manifest.");
        }
        ofs.write(json.data(), static_cast<std::streamsize>(json.size()));
        ofs.flush();
        if (!ofs.good()) {
            throw VaultException(VaultErrorKind::IoFailure, "Failed flushing vault manifest.");
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(tmpPath.c_str(), manifestPath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw VaultException(VaultErrorKind::IoFailure, "Atomic manifest MoveFileExW failed.");
    }
#else
    ::chmod(tmpPath.string().c_str(), 0600);
    std::error_code ec;
    fs::rename(tmpPath, fs::path(manifestPath_), ec);
    if (ec) {
        throw VaultException(VaultErrorKind::IoFailure, "Atomic manifest rename failed.");
    }
#endif
}

std::array<uint8_t, 32> VaultManager::copyUnlockedPrivateKey() const {
    std::lock_guard<std::mutex> guard(stateMutex_);
    if (unlockedPrivateKey_.size() != 32) {
        throw VaultException(VaultErrorKind::VaultLocked, "Vault is locked. Enter your password to unlock.");
    }
    std::array<uint8_t, 32> copy{};
    std::memcpy(copy.data(), unlockedPrivateKey_.data(), 32);
    return copy;
}

} // namespace maxwatch
