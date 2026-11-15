#pragma once

#include <cstdint>
#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace maxwatch {

enum class VaultErrorKind {
    VaultNotInitialized,
    VaultAlreadyInitialized,
    VaultLocked,
    InvalidPassword,
    PasswordTooShort,
    CorruptedManifest,
    CorruptedClipHeader,
    IoFailure,
    CryptoFailure
};

class VaultException : public std::runtime_error {
public:
    VaultErrorKind kind;
    VaultException(VaultErrorKind k, const std::string& msg)
        : std::runtime_error(msg), kind(k) {}
};

struct UUIDBytes {
    std::array<uint8_t, 16> bytes{};

    static UUIDBytes generateRandom();
    std::string toString() const;
    std::string shortPrefix8() const;
    bool operator==(const UUIDBytes& o) const { return bytes == o.bytes; }
};

struct VaultManifest {
    int version = 1;
    int pbkdf2Iterations = 310000;
    std::string pbkdf2SaltBase64;
    std::string publicKeyBase64;
    std::string wrappedPrivateKeyBase64;
    std::string createdAtISO8601;

    std::string toJson() const;
    static VaultManifest fromJson(const std::string& jsonText);
};

enum class VaultSegmentKind : uint8_t {
    Initialization = 1,
    MediaFragment = 2
};

struct ClipFileHeader {
    static constexpr size_t kByteSize = 128;
    static constexpr uint16_t kCurrentVersion = 1;

    uint16_t version = kCurrentVersion;
    uint8_t flags = 0;
    UUIDBytes clipUUID;
    int64_t createdUnixMs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::array<uint8_t, 32> ephemeralPublicKeyRaw{};
    std::array<uint8_t, 32> hkdfSalt{};
    uint64_t durationMs = 0;

    bool hasAudio() const { return (flags & 0x01) != 0; }
    bool cleanlyFinalized() const { return (flags & 0x02) != 0; }

    std::vector<uint8_t> serialize() const;
    static ClipFileHeader parse(const uint8_t* data, size_t len);
};

struct EncryptedClipSummary {
    std::wstring filePath;
    std::wstring fileName;
    UUIDBytes clipUUID;
    int64_t createdUnixMs = 0;
    int64_t fileSizeBytes = 0;
    int width = 0;
    int height = 0;
    bool hasAudio = false;
    bool cleanlyFinalized = false;
    double headerDurationSeconds = 0.0;
    int validChunkCount = 0;
    double estimatedDurationSeconds = 0.0;
};

namespace crypto {

void secureZero(void* ptr, size_t len);
void secureZeroVec(std::vector<uint8_t>& v);

void randomBytes(uint8_t* out, size_t len);
std::vector<uint8_t> randomBytesVec(size_t len);

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len);
std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen);
std::array<uint8_t, 32> derivePBKDF2Sha256(const std::string& password, const uint8_t* salt, size_t saltLen, int iterations);
std::array<uint8_t, 32> hkdfSha256(
    const uint8_t* ikm, size_t ikmLen,
    const uint8_t* salt, size_t saltLen,
    const uint8_t* info, size_t infoLen
);

// RFC 7748 Constant-Time Curve25519 (X25519)
std::array<uint8_t, 32> x25519PublicFromPrivate(const std::array<uint8_t, 32>& scalar);
std::array<uint8_t, 32> x25519SharedSecret(
    const std::array<uint8_t, 32>& myPrivate,
    const std::array<uint8_t, 32>& peerPublic
);

// AES-256-GCM Combined Format: [12-byte nonce || ciphertext || 16-byte tag] (matches Apple CryptoKit byte-for-byte)
std::vector<uint8_t> aes256GcmSeal(
    const std::array<uint8_t, 32>& key,
    const uint8_t* plaintext, size_t plaintextLen,
    const uint8_t* aad, size_t aadLen,
    const uint8_t* explicitNonce12 = nullptr
);

std::vector<uint8_t> aes256GcmOpen(
    const std::array<uint8_t, 32>& key,
    const uint8_t* combined, size_t combinedLen,
    const uint8_t* aad, size_t aadLen
);

std::array<uint8_t, 32> deriveClipKey(
    const std::array<uint8_t, 32>& sharedSecret,
    const std::array<uint8_t, 32>& hkdfSalt,
    const UUIDBytes& clipUUID
);

std::vector<uint8_t> makeChunkAAD(const UUIDBytes& clipUUID, uint64_t chunkIndex, VaultSegmentKind kind);

std::string base64Encode(const uint8_t* data, size_t len);
std::vector<uint8_t> base64Decode(const std::string& b64);

} // namespace crypto

class VaultClipFileWriter {
public:
    VaultClipFileWriter(const std::wstring& path, const ClipFileHeader& header, const std::array<uint8_t, 32>& clipKey);
    ~VaultClipFileWriter();

    VaultClipFileWriter(const VaultClipFileWriter&) = delete;
    VaultClipFileWriter& operator=(const VaultClipFileWriter&) = delete;

    const std::wstring& filePath() const { return filePath_; }
    const ClipFileHeader& header() const { return header_; }
    int writtenChunkCount() const;

    void appendSegment(const uint8_t* plaintext, size_t len, VaultSegmentKind kind);
    void appendSegment(const std::vector<uint8_t>& plaintext, VaultSegmentKind kind) {
        appendSegment(plaintext.data(), plaintext.size(), kind);
    }
    void finalizeAndClose(double durationSeconds, bool cleanlyFinalized);

private:
    void writeAllAt(const uint8_t* data, size_t len, uint64_t offset);
    void writeAllAtEnd(const uint8_t* data, size_t len);
    void flushDisk();
    void closeHandle();

    std::wstring filePath_;
    ClipFileHeader header_;
    std::array<uint8_t, 32> clipKey_{};
    bool hasKey_ = false;
    bool isClosed_ = false;
    uint64_t nextChunkIndex_ = 0;
    mutable std::mutex mutex_;
#ifdef _WIN32
    void* fileHandle_ = reinterpret_cast<void*>(-1);
#else
    int fd_ = -1;
#endif
};

struct IndexedEncryptedChunk {
    uint64_t chunkIndex = 0;
    VaultSegmentKind kind = VaultSegmentKind::Initialization;
    uint64_t filePayloadOffset = 0;
    size_t combinedLength = 0;
    int64_t plaintextOffset = 0;
    size_t plaintextLength = 0;
};

class VaultClipReader {
public:
    VaultClipReader(const std::wstring& filePath, const std::array<uint8_t, 32>& clipKey);
    ~VaultClipReader();

    VaultClipReader(const VaultClipReader&) = delete;
    VaultClipReader& operator=(const VaultClipReader&) = delete;

    const std::wstring& filePath() const { return filePath_; }
    const ClipFileHeader& header() const { return header_; }
    const std::vector<IndexedEncryptedChunk>& chunks() const { return chunks_; }
    int64_t totalPlaintextLength() const { return totalPlaintextLength_; }
    int mediaFragmentCount() const { return mediaFragmentCount_; }

    std::vector<uint8_t> readPlaintextRange(int64_t offset, size_t length);

    static std::vector<uint8_t> preadExactStatic(
#ifdef _WIN32
        void* handle,
#else
        int fd,
#endif
        uint64_t offset,
        size_t length
    );

private:
    std::vector<uint8_t> decryptedChunk(const IndexedEncryptedChunk& chunk);
    static std::vector<uint8_t> decryptChunkStatic(
#ifdef _WIN32
        void* handle,
#else
        int fd,
#endif
        const IndexedEncryptedChunk& chunk,
        const UUIDBytes& clipUUID,
        const std::array<uint8_t, 32>& key
    );

    std::wstring filePath_;
    ClipFileHeader header_;
    std::vector<IndexedEncryptedChunk> chunks_;
    int64_t totalPlaintextLength_ = 0;
    int mediaFragmentCount_ = 0;
    std::array<uint8_t, 32> clipKey_{};
#ifdef _WIN32
    void* fileHandle_ = reinterpret_cast<void*>(-1);
#else
    int fd_ = -1;
#endif

    std::mutex cacheMutex_;
    std::map<uint64_t, std::vector<uint8_t>> decryptedCache_;
    std::vector<uint64_t> cacheOrder_;
    static constexpr size_t kMaxCachedChunks = 28;
};

class VaultManager {
public:
    explicit VaultManager(const std::wstring& customVaultDir = L"");
    ~VaultManager();

    const std::wstring& vaultDirectoryPath() const { return vaultDir_; }
    const std::wstring& clipsDirectoryPath() const { return clipsDir_; }
    const std::wstring& manifestFilePath() const { return manifestPath_; }

    bool isInitialized() const;
    bool isUnlocked() const;

    void initializeVault(const std::string& password);
    bool unlock(const std::string& password);
    void lock();
    void changePassword(const std::string& currentPassword, const std::string& newPassword);
    void resetVaultDeletingAllClips();

    std::unique_ptr<VaultClipFileWriter> createClipWriter(int width, int height, bool hasAudio);
    std::unique_ptr<VaultClipReader> openClipReader(const std::wstring& clipPath);

    int countVaultClips() const;
    std::vector<EncryptedClipSummary> listSummaries(const std::wstring& excludeActivePath = L"") const;

    void secureDeleteClip(const std::wstring& clipPath);
    std::wstring exportClipToDirectory(const std::wstring& clipPath, const std::wstring& directoryPath);
    void exportClipAsMP4(const std::wstring& clipPath, const std::wstring& destinationPath);

    bool isPathInsideVault(const std::wstring& candidatePath) const;

private:
    void ensureDirectories() const;
    VaultManifest loadManifest() const;
    void writeManifestAtomically(const VaultManifest& manifest) const;
    std::array<uint8_t, 32> copyUnlockedPrivateKey() const;

    std::wstring vaultDir_;
    std::wstring clipsDir_;
    std::wstring manifestPath_;

    mutable std::mutex stateMutex_;
    std::vector<uint8_t> unlockedPrivateKey_;
    bool privateKeyMemoryLocked_ = false;
    static constexpr int kPbkdf2Iterations = 310000;
};

} // namespace maxwatch
