import CommonCrypto
import CryptoKit
import Darwin
import Foundation

// MARK: - Vault Errors

enum VaultError: LocalizedError {
    case vaultNotInitialized
    case vaultAlreadyInitialized
    case vaultLocked
    case invalidPassword
    case passwordTooShort
    case corruptedManifest
    case corruptedClipHeader
    case ioFailure(String)
    case cryptoFailure(String)

    var errorDescription: String? {
        switch self {
        case .vaultNotInitialized:
            return "Vault password has not been set yet."
        case .vaultAlreadyInitialized:
            return "Vault is already initialized."
        case .vaultLocked:
            return "Vault is locked. Enter your password to unlock."
        case .invalidPassword:
            return "Incorrect vault password."
        case .passwordTooShort:
            return "Password must be at least 4 characters."
        case .corruptedManifest:
            return "Vault manifest is unreadable or damaged."
        case .corruptedClipHeader:
            return "Encrypted clip header is unreadable or damaged."
        case .ioFailure(let msg):
            return "Disk I/O error: \(msg)"
        case .cryptoFailure(let msg):
            return "Encryption error: \(msg)"
        }
    }
}

// MARK: - Manifest & Header Models

struct VaultManifest: Codable {
    let version: Int
    let pbkdf2Iterations: Int
    let pbkdf2SaltBase64: String
    let publicKeyBase64: String
    let wrappedPrivateKeyBase64: String
    let createdAtISO8601: String
}

enum VaultSegmentKind: UInt8 {
    case initialization = 1
    case mediaFragment = 2
}

struct ClipFileHeader {
    static let byteSize = 128
    static let magicBytes: [UInt8] = [0x4D, 0x57, 0x56, 0x31] // "MWV1"
    static let currentVersion: UInt16 = 1

    let version: UInt16
    var flags: UInt8
    let clipUUID: UUID
    let createdUnixMs: Int64
    let width: UInt32
    let height: UInt32
    let ephemeralPublicKeyRaw: Data // 32 bytes
    let hkdfSalt: Data              // 32 bytes
    var durationMs: UInt64

    var hasAudio: Bool { (flags & 0x01) != 0 }
    var cleanlyFinalized: Bool { (flags & 0x02) != 0 }

    func serialize() -> Data {
        var data = Data(capacity: Self.byteSize)
        data.append(contentsOf: Self.magicBytes)
        var verBE = version.bigEndian
        withUnsafeBytes(of: &verBE) { data.append(contentsOf: $0) }
        data.append(flags)
        data.append(0) // reserved

        var uuidBytes = clipUUID.uuid
        withUnsafeBytes(of: &uuidBytes) { data.append(contentsOf: $0) }

        var createdBE = createdUnixMs.bigEndian
        withUnsafeBytes(of: &createdBE) { data.append(contentsOf: $0) }

        var widthBE = width.bigEndian
        withUnsafeBytes(of: &widthBE) { data.append(contentsOf: $0) }

        var heightBE = height.bigEndian
        withUnsafeBytes(of: &heightBE) { data.append(contentsOf: $0) }

        precondition(ephemeralPublicKeyRaw.count == 32)
        precondition(hkdfSalt.count == 32)
        data.append(ephemeralPublicKeyRaw)
        data.append(hkdfSalt)

        // Bytes [0..103] integrity digest -> [104..119]
        let digest = SHA256.hash(data: data)
        data.append(contentsOf: digest.prefix(16))

        var durBE = durationMs.bigEndian
        withUnsafeBytes(of: &durBE) { data.append(contentsOf: $0) }

        precondition(data.count == Self.byteSize)
        return data
    }

    static func parse(from data: Data) throws -> ClipFileHeader {
        guard data.count >= byteSize else {
            throw VaultError.corruptedClipHeader
        }
        let magic = [data[0], data[1], data[2], data[3]]
        guard magic == magicBytes else {
            throw VaultError.corruptedClipHeader
        }

        let expectedDigest = SHA256.hash(data: data.subdata(in: 0..<104))
        let storedDigest = data.subdata(in: 104..<120)
        guard Data(expectedDigest.prefix(16)) == storedDigest else {
            throw VaultError.corruptedClipHeader
        }

        func readUInt16BE(_ offset: Int) -> UInt16 {
            var v: UInt16 = 0
            _ = withUnsafeMutableBytes(of: &v) { data.copyBytes(to: $0, from: offset..<(offset + 2)) }
            return UInt16(bigEndian: v)
        }
        func readUInt32BE(_ offset: Int) -> UInt32 {
            var v: UInt32 = 0
            _ = withUnsafeMutableBytes(of: &v) { data.copyBytes(to: $0, from: offset..<(offset + 4)) }
            return UInt32(bigEndian: v)
        }
        func readInt64BE(_ offset: Int) -> Int64 {
            var v: Int64 = 0
            _ = withUnsafeMutableBytes(of: &v) { data.copyBytes(to: $0, from: offset..<(offset + 8)) }
            return Int64(bigEndian: v)
        }
        func readUInt64BE(_ offset: Int) -> UInt64 {
            var v: UInt64 = 0
            _ = withUnsafeMutableBytes(of: &v) { data.copyBytes(to: $0, from: offset..<(offset + 8)) }
            return UInt64(bigEndian: v)
        }

        let ver = readUInt16BE(4)
        guard ver == currentVersion else {
            throw VaultError.corruptedClipHeader
        }
        let flags = data[6]

        var uuidT: uuid_t = (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        _ = withUnsafeMutableBytes(of: &uuidT) { data.copyBytes(to: $0, from: 8..<24) }
        let uuid = UUID(uuid: uuidT)

        let createdMs = readInt64BE(24)
        let width = readUInt32BE(32)
        let height = readUInt32BE(36)
        let ephPub = data.subdata(in: 40..<72)
        let salt = data.subdata(in: 72..<104)
        let durationMs = readUInt64BE(120)

        return ClipFileHeader(
            version: ver,
            flags: flags,
            clipUUID: uuid,
            createdUnixMs: createdMs,
            width: width,
            height: height,
            ephemeralPublicKeyRaw: ephPub,
            hkdfSalt: salt,
            durationMs: durationMs
        )
    }
}

struct EncryptedClipSummary {
    let url: URL
    let clipUUID: UUID
    let createdAt: Date
    let fileSizeBytes: Int64
    let width: Int
    let height: Int
    let hasAudio: Bool
    let cleanlyFinalized: Bool
    let headerDurationSeconds: Double
    let validChunkCount: Int
    let estimatedDurationSeconds: Double
}

// MARK: - Cryptographic Helpers

enum VaultCryptoPrimitives {
    static let wrapAAD = Data("MaxWatchVaultKeyV1".utf8)
    static let clipHKDFInfo = Data("MaxWatchClipAES256GCM_V1".utf8)
    static let chunkAADPrefix = Data("MWV1Chunk".utf8)

    static func derivePBKDF2Key(password: String, salt: Data, iterations: Int) throws -> SymmetricKey {
        let passwordData = Data(password.utf8)
        guard !passwordData.isEmpty else {
            throw VaultError.passwordTooShort
        }
        var derived = Data(count: 32)
        let status = derived.withUnsafeMutableBytes { derivedBytes -> Int32 in
            salt.withUnsafeBytes { saltBytes -> Int32 in
                passwordData.withUnsafeBytes { passBytes -> Int32 in
                    CCKeyDerivationPBKDF(
                        CCPBKDFAlgorithm(kCCPBKDF2),
                        passBytes.baseAddress?.assumingMemoryBound(to: Int8.self),
                        passwordData.count,
                        saltBytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                        salt.count,
                        CCPseudoRandomAlgorithm(kCCPRFHmacAlgSHA256),
                        UInt32(iterations),
                        derivedBytes.baseAddress?.assumingMemoryBound(to: UInt8.self),
                        32
                    )
                }
            }
        }
        guard status == kCCSuccess else {
            throw VaultError.cryptoFailure("PBKDF2 derivation failed (\(status))")
        }
        let key = SymmetricKey(data: derived)
        secureZero(&derived)
        return key
    }

    static func deriveClipKey(sharedSecret: SharedSecret, salt: Data, clipUUID: UUID) -> SymmetricKey {
        var info = Data(capacity: clipHKDFInfo.count + 16)
        info.append(clipHKDFInfo)
        var u = clipUUID.uuid
        withUnsafeBytes(of: &u) { info.append(contentsOf: $0) }
        return sharedSecret.hkdfDerivedSymmetricKey(
            using: SHA256.self,
            salt: salt,
            sharedInfo: info,
            outputByteCount: 32
        )
    }

    static func makeChunkAAD(clipUUID: UUID, chunkIndex: UInt64, kind: VaultSegmentKind) -> Data {
        var aad = Data(capacity: chunkAADPrefix.count + 16 + 8 + 1)
        aad.append(chunkAADPrefix)
        var u = clipUUID.uuid
        withUnsafeBytes(of: &u) { aad.append(contentsOf: $0) }
        var idxBE = chunkIndex.bigEndian
        withUnsafeBytes(of: &idxBE) { aad.append(contentsOf: $0) }
        aad.append(kind.rawValue)
        return aad
    }

    static func randomBytes(_ count: Int) throws -> Data {
        var bytes = Data(count: count)
        let result = bytes.withUnsafeMutableBytes { ptr in
            SecRandomCopyBytes(kSecRandomDefault, count, ptr.baseAddress!)
        }
        guard result == errSecSuccess else {
            throw VaultError.cryptoFailure("SecRandomCopyBytes failed (\(result))")
        }
        return bytes
    }

    static func secureZero(_ data: inout Data) {
        guard !data.isEmpty else { return }
        data.withUnsafeMutableBytes { ptr in
            if let base = ptr.baseAddress {
                memset_s(base, ptr.count, 0, ptr.count)
            }
        }
    }
}

// MARK: - Power-Cut-Safe Encrypted Clip File Writer

final class VaultClipFileWriter {
    let fileURL: URL
    private(set) var header: ClipFileHeader
    private var clipKey: SymmetricKey?
    private var fd: Int32 = -1
    private var nextChunkIndex: UInt64 = 0
    private let lock = NSLock()
    private(set) var isClosed: Bool = false

    var writtenChunkCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return Int(nextChunkIndex)
    }

    init(fileURL: URL, header: ClipFileHeader, clipKey: SymmetricKey) throws {
        self.fileURL = fileURL
        self.header = header
        self.clipKey = clipKey

        let opened = fileURL.path.withCString { cPath in
            Darwin.open(cPath, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR)
        }
        guard opened >= 0 else {
            let code = errno
            throw VaultError.ioFailure("Unable to create vault clip (\(String(cString: strerror(code))))")
        }
        self.fd = opened
        _ = fcntl(fd, F_NOCACHE, 1)

        let headerBytes = header.serialize()
        try writeAll(headerBytes, atOffset: 0)
        if Darwin.fsync(fd) != 0 {
            let code = errno
            Darwin.close(fd)
            fd = -1
            throw VaultError.ioFailure("Header fsync failed (\(String(cString: strerror(code))))")
        }
    }

    deinit {
        if !isClosed {
            try? finalizeAndClose(durationSeconds: 0, cleanlyFinalized: false)
        }
    }

    /// Encrypts and fsyncs a single 1-second fMP4 segment directly to disk.
    /// Zero plaintext ever touches the filesystem.
    func appendSegment(_ plaintext: Data, kind: VaultSegmentKind) throws {
        lock.lock()
        defer { lock.unlock() }

        guard !isClosed, fd >= 0, let key = clipKey else {
            throw VaultError.ioFailure("Vault clip writer is already closed.")
        }
        guard !plaintext.isEmpty else { return }

        let idx = nextChunkIndex
        let aad = VaultCryptoPrimitives.makeChunkAAD(clipUUID: header.clipUUID, chunkIndex: idx, kind: kind)
        let nonce = AES.GCM.Nonce()
        let sealed = try AES.GCM.seal(plaintext, using: key, nonce: nonce, authenticating: aad)
        guard let combined = sealed.combined else {
            throw VaultError.cryptoFailure("Failed to assemble AES-GCM combined box.")
        }
        guard combined.count <= 64_000_000 else {
            throw VaultError.cryptoFailure("Segment exceeds 64 MB safety bound.")
        }

        var packet = Data(capacity: 5 + combined.count)
        var lenBE = UInt32(combined.count).bigEndian
        withUnsafeBytes(of: &lenBE) { packet.append(contentsOf: $0) }
        packet.append(kind.rawValue)
        packet.append(combined)

        try writeAllAtEnd(packet)
        if Darwin.fsync(fd) != 0 {
            let code = errno
            throw VaultError.ioFailure("Segment fsync failed (\(String(cString: strerror(code))))")
        }
        nextChunkIndex += 1
    }

    func finalizeAndClose(durationSeconds: Double, cleanlyFinalized: Bool) throws {
        lock.lock()
        defer { lock.unlock() }

        guard !isClosed else { return }
        isClosed = true
        clipKey = nil // Release per-clip symmetric key from RAM immediately

        guard fd >= 0 else { return }
        defer {
            Darwin.close(fd)
            fd = -1
        }

        if cleanlyFinalized {
            header.flags |= 0x02
        }
        if durationSeconds > 0 {
            header.durationMs = UInt64((durationSeconds * 1000.0).rounded())
        }
        let updatedHeader = header.serialize()
        try writeAll(updatedHeader, atOffset: 0)
        if fcntl(fd, F_FULLFSYNC) != 0 {
            _ = Darwin.fsync(fd)
        }
    }

    private func writeAll(_ data: Data, atOffset offset: off_t) throws {
        var totalWritten = 0
        try data.withUnsafeBytes { rawBuf in
            guard let base = rawBuf.baseAddress else { return }
            while totalWritten < data.count {
                let n = Darwin.pwrite(fd, base.advanced(by: totalWritten), data.count - totalWritten, offset + off_t(totalWritten))
                if n < 0 {
                    if errno == EINTR { continue }
                    throw VaultError.ioFailure("pwrite failed (\(String(cString: strerror(errno))))")
                }
                totalWritten += n
            }
        }
    }

    private func writeAllAtEnd(_ data: Data) throws {
        let endOffset = Darwin.lseek(fd, 0, SEEK_END)
        guard endOffset >= 0 else {
            throw VaultError.ioFailure("lseek failed (\(String(cString: strerror(errno))))")
        }
        try writeAll(data, atOffset: endOffset)
    }
}

// MARK: - Fault-Tolerant Encrypted Clip Reader (Recovers Power-Cut Clips Instantly)

struct IndexedEncryptedChunk {
    let chunkIndex: UInt64
    let kind: VaultSegmentKind
    let filePayloadOffset: UInt64
    let combinedLength: Int
    let plaintextOffset: Int64
    let plaintextLength: Int
}

final class VaultClipReader {
    let fileURL: URL
    let header: ClipFileHeader
    let chunks: [IndexedEncryptedChunk]
    let totalPlaintextLength: Int64
    let mediaFragmentCount: Int
    private let clipKey: SymmetricKey
    private let fd: Int32
    private let cacheLock = NSLock()
    private var decryptedCache: [UInt64: Data] = [:]
    private var cacheOrder: [UInt64] = []
    private let maxCachedChunks = 28

    init(fileURL: URL, clipKey: SymmetricKey) throws {
        self.fileURL = fileURL
        self.clipKey = clipKey

        let opened = fileURL.path.withCString { cPath in
            Darwin.open(cPath, O_RDONLY | O_CLOEXEC)
        }
        guard opened >= 0 else {
            throw VaultError.ioFailure("Cannot open encrypted clip (\(String(cString: strerror(errno))))")
        }
        self.fd = opened

        var st = stat()
        guard Darwin.fstat(opened, &st) == 0 else {
            Darwin.close(opened)
            throw VaultError.ioFailure("fstat failed")
        }
        let fileSize = UInt64(max(0, st.st_size))
        guard fileSize >= UInt64(ClipFileHeader.byteSize) else {
            Darwin.close(opened)
            throw VaultError.corruptedClipHeader
        }

        let headerData = try Self.preadExact(fd: opened, offset: 0, length: ClipFileHeader.byteSize)
        do {
            self.header = try ClipFileHeader.parse(from: headerData)
        } catch {
            Darwin.close(opened)
            throw error
        }

        // Index all valid chunks sequentially. If hard power loss truncated the final chunk,
        // stop cleanly at the last valid chunk so all preceding 1s segments remain 100% playable.
        var cursor = UInt64(ClipFileHeader.byteSize)
        var indexed: [IndexedEncryptedChunk] = []
        var virtualPos: Int64 = 0
        var idx: UInt64 = 0

        while cursor + 5 <= fileSize {
            guard let prefixData = try? Self.preadExact(fd: opened, offset: cursor, length: 5),
                  prefixData.count == 5 else {
                break
            }
            var lenBE: UInt32 = 0
            _ = withUnsafeMutableBytes(of: &lenBE) { prefixData.copyBytes(to: $0, from: 0..<4) }
            let combinedLen = Int(UInt32(bigEndian: lenBE))
            let rawKind = prefixData[4]

            guard combinedLen >= 28,
                  combinedLen <= 64_000_000,
                  let kind = VaultSegmentKind(rawValue: rawKind),
                  cursor + 5 + UInt64(combinedLen) <= fileSize else {
                // Truncated tail chunk from sudden power loss -> stop cleanly here.
                break
            }

            let plainLen = combinedLen - 28
            let entry = IndexedEncryptedChunk(
                chunkIndex: idx,
                kind: kind,
                filePayloadOffset: cursor + 5,
                combinedLength: combinedLen,
                plaintextOffset: virtualPos,
                plaintextLength: plainLen
            )
            indexed.append(entry)
            virtualPos += Int64(plainLen)
            cursor += 5 + UInt64(combinedLen)
            idx += 1
        }

        // Authenticate Chunk 0 and the final chunk immediately so wrong keys or half-written
        // power-cut tail bytes (where length happened to fit file size) are caught cleanly.
        while let last = indexed.last {
            if (try? Self.decryptChunkStatic(
                fd: opened,
                chunk: last,
                clipUUID: header.clipUUID,
                key: clipKey
            )) != nil {
                break
            } else {
                virtualPos -= Int64(last.plaintextLength)
                indexed.removeLast()
            }
        }

        if let first = indexed.first {
            guard (try? Self.decryptChunkStatic(
                fd: opened,
                chunk: first,
                clipUUID: header.clipUUID,
                key: clipKey
            )) != nil else {
                Darwin.close(opened)
                throw VaultError.cryptoFailure("Failed to authenticate initialization segment.")
            }
        }

        self.chunks = indexed
        self.totalPlaintextLength = virtualPos
        self.mediaFragmentCount = indexed.filter { $0.kind == .mediaFragment }.count
    }

    deinit {
        if fd >= 0 {
            Darwin.close(fd)
        }
        cacheLock.lock()
        for k in decryptedCache.keys {
            if var d = decryptedCache[k] {
                VaultCryptoPrimitives.secureZero(&d)
            }
        }
        decryptedCache.removeAll()
        cacheOrder.removeAll()
        cacheLock.unlock()
    }

    /// Reads arbitrary virtual MP4 byte ranges in RAM by decrypting only overlapping 1s chunks.
    func readPlaintextRange(offset: Int64, length: Int) throws -> Data {
        guard length > 0, offset >= 0, offset < totalPlaintextLength else {
            return Data()
        }
        let clampedEnd = min(totalPlaintextLength, offset + Int64(length))
        let targetLen = Int(clampedEnd - offset)
        var out = Data(capacity: targetLen)

        // Binary search first chunk whose end > offset
        var lo = 0
        var hi = chunks.count - 1
        var startIdx = chunks.count
        while lo <= hi {
            let mid = (lo + hi) / 2
            let c = chunks[mid]
            if c.plaintextOffset + Int64(c.plaintextLength) > offset {
                startIdx = mid
                hi = mid - 1
            } else {
                lo = mid + 1
            }
        }

        var idx = startIdx
        while idx < chunks.count {
            let c = chunks[idx]
            if c.plaintextOffset >= clampedEnd { break }
            let plain = try decryptedChunk(c)

            let copyStart = Int(max(0, offset - c.plaintextOffset))
            let copyEnd = Int(min(Int64(c.plaintextLength), clampedEnd - c.plaintextOffset))
            if copyEnd > copyStart {
                out.append(plain.subdata(in: copyStart..<copyEnd))
            }
            idx += 1
        }
        return out
    }

    private func decryptedChunk(_ chunk: IndexedEncryptedChunk) throws -> Data {
        cacheLock.lock()
        if let hit = decryptedCache[chunk.chunkIndex] {
            if let pos = cacheOrder.firstIndex(of: chunk.chunkIndex) {
                cacheOrder.remove(at: pos)
            }
            cacheOrder.append(chunk.chunkIndex)
            cacheLock.unlock()
            return hit
        }
        cacheLock.unlock()

        let decrypted = try Self.decryptChunkStatic(
            fd: fd,
            chunk: chunk,
            clipUUID: header.clipUUID,
            key: clipKey
        )

        cacheLock.lock()
        decryptedCache[chunk.chunkIndex] = decrypted
        cacheOrder.append(chunk.chunkIndex)
        while cacheOrder.count > maxCachedChunks {
            let evicted = cacheOrder.removeFirst()
            if var old = decryptedCache.removeValue(forKey: evicted) {
                VaultCryptoPrimitives.secureZero(&old)
            }
        }
        cacheLock.unlock()
        return decrypted
    }

    private static func decryptChunkStatic(
        fd: Int32,
        chunk: IndexedEncryptedChunk,
        clipUUID: UUID,
        key: SymmetricKey
    ) throws -> Data {
        let combined = try preadExact(fd: fd, offset: chunk.filePayloadOffset, length: chunk.combinedLength)
        let box = try AES.GCM.SealedBox(combined: combined)
        let aad = VaultCryptoPrimitives.makeChunkAAD(clipUUID: clipUUID, chunkIndex: chunk.chunkIndex, kind: chunk.kind)
        return try AES.GCM.open(box, using: key, authenticating: aad)
    }

    fileprivate static func preadExact(fd: Int32, offset: UInt64, length: Int) throws -> Data {
        var buf = Data(count: length)
        var totalRead = 0
        try buf.withUnsafeMutableBytes { rawBuf in
            guard let base = rawBuf.baseAddress else { return }
            while totalRead < length {
                let n = Darwin.pread(fd, base.advanced(by: totalRead), length - totalRead, off_t(offset) + off_t(totalRead))
                if n < 0 {
                    if errno == EINTR { continue }
                    throw VaultError.ioFailure("pread failed (\(String(cString: strerror(errno))))")
                }
                if n == 0 {
                    throw VaultError.ioFailure("Unexpected EOF in vault clip")
                }
                totalRead += n
            }
        }
        return buf
    }
}

// MARK: - Vault Manager (Hybrid Public-Key Capture + Password-Locked Playback)

final class VaultManager {
    let vaultDirectoryURL: URL
    let clipsDirectoryURL: URL
    let manifestURL: URL

    private let stateLock = NSLock()
    private var unlockedPrivateKeyBytes: Data? = nil
    private let pbkdf2Iterations = 310_000

    init(customVaultDirectory: URL? = nil) {
        if let custom = customVaultDirectory {
            self.vaultDirectoryURL = custom
        } else {
            let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            self.vaultDirectoryURL = appSupport
                .appendingPathComponent("MaxWatch", isDirectory: true)
                .appendingPathComponent("Vault", isDirectory: true)
        }
        self.clipsDirectoryURL = vaultDirectoryURL.appendingPathComponent("Clips", isDirectory: true)
        self.manifestURL = vaultDirectoryURL.appendingPathComponent("vault_manifest.json", isDirectory: false)
        ensureDirectories()
    }

    deinit {
        lock()
    }

    private func ensureDirectories() {
        let attrs: [FileAttributeKey: Any] = [.posixPermissions: NSNumber(value: Int16(0o700))]
        try? FileManager.default.createDirectory(at: vaultDirectoryURL, withIntermediateDirectories: true, attributes: attrs)
        try? FileManager.default.createDirectory(at: clipsDirectoryURL, withIntermediateDirectories: true, attributes: attrs)
    }

    var isInitialized: Bool {
        FileManager.default.fileExists(atPath: manifestURL.path)
    }

    var isUnlocked: Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        return unlockedPrivateKeyBytes != nil
    }

    func initializeVault(password: String) throws {
        guard password.count >= 4 else {
            throw VaultError.passwordTooShort
        }
        ensureDirectories()
        guard !isInitialized else {
            throw VaultError.vaultAlreadyInitialized
        }

        let salt = try VaultCryptoPrimitives.randomBytes(32)
        let kek = try VaultCryptoPrimitives.derivePBKDF2Key(password: password, salt: salt, iterations: pbkdf2Iterations)

        let privateKey = Curve25519.KeyAgreement.PrivateKey()
        var privRaw = privateKey.rawRepresentation
        defer { VaultCryptoPrimitives.secureZero(&privRaw) }

        let sealed = try AES.GCM.seal(privRaw, using: kek, authenticating: VaultCryptoPrimitives.wrapAAD)
        guard let combined = sealed.combined else {
            throw VaultError.cryptoFailure("Failed to wrap vault private key.")
        }

        let iso = ISO8601DateFormatter().string(from: Date())
        let manifest = VaultManifest(
            version: 1,
            pbkdf2Iterations: pbkdf2Iterations,
            pbkdf2SaltBase64: salt.base64EncodedString(),
            publicKeyBase64: privateKey.publicKey.rawRepresentation.base64EncodedString(),
            wrappedPrivateKeyBase64: combined.base64EncodedString(),
            createdAtISO8601: iso
        )

        try writeManifestAtomically(manifest)

        // Security invariant: keep vault LOCKED immediately after password creation so arming
        // and saving clips never leaves Play All or Delete exposed if someone enters the room.
        stateLock.lock()
        if var existing = unlockedPrivateKeyBytes {
            VaultCryptoPrimitives.secureZero(&existing)
        }
        unlockedPrivateKeyBytes = nil
        stateLock.unlock()
    }

    @discardableResult
    func unlock(password: String) throws -> Bool {
        let manifest = try loadManifest()
        guard let salt = Data(base64Encoded: manifest.pbkdf2SaltBase64),
              let wrapped = Data(base64Encoded: manifest.wrappedPrivateKeyBase64),
              let expectedPub = Data(base64Encoded: manifest.publicKeyBase64) else {
            throw VaultError.corruptedManifest
        }

        let kek = try VaultCryptoPrimitives.derivePBKDF2Key(
            password: password,
            salt: salt,
            iterations: manifest.pbkdf2Iterations
        )

        let box: AES.GCM.SealedBox
        do {
            box = try AES.GCM.SealedBox(combined: wrapped)
        } catch {
            throw VaultError.corruptedManifest
        }

        let privRaw: Data
        do {
            privRaw = try AES.GCM.open(box, using: kek, authenticating: VaultCryptoPrimitives.wrapAAD)
        } catch {
            throw VaultError.invalidPassword
        }

        let privKey = try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: privRaw)
        guard privKey.publicKey.rawRepresentation == expectedPub else {
            throw VaultError.corruptedManifest
        }

        stateLock.lock()
        if var existing = unlockedPrivateKeyBytes {
            VaultCryptoPrimitives.secureZero(&existing)
        }
        unlockedPrivateKeyBytes = privRaw
        stateLock.unlock()
        return true
    }

    func lock() {
        stateLock.lock()
        if var priv = unlockedPrivateKeyBytes {
            VaultCryptoPrimitives.secureZero(&priv)
        }
        unlockedPrivateKeyBytes = nil
        stateLock.unlock()
    }

    func changePassword(currentPassword: String, newPassword: String) throws {
        guard newPassword.count >= 4 else {
            throw VaultError.passwordTooShort
        }
        let manifest = try loadManifest()
        guard let oldSalt = Data(base64Encoded: manifest.pbkdf2SaltBase64),
              let oldWrapped = Data(base64Encoded: manifest.wrappedPrivateKeyBase64) else {
            throw VaultError.corruptedManifest
        }

        let oldKEK = try VaultCryptoPrimitives.derivePBKDF2Key(
            password: currentPassword,
            salt: oldSalt,
            iterations: manifest.pbkdf2Iterations
        )
        let oldBox = try AES.GCM.SealedBox(combined: oldWrapped)
        var privRaw: Data
        do {
            privRaw = try AES.GCM.open(oldBox, using: oldKEK, authenticating: VaultCryptoPrimitives.wrapAAD)
        } catch {
            throw VaultError.invalidPassword
        }
        defer { VaultCryptoPrimitives.secureZero(&privRaw) }

        let newSalt = try VaultCryptoPrimitives.randomBytes(32)
        let newKEK = try VaultCryptoPrimitives.derivePBKDF2Key(
            password: newPassword,
            salt: newSalt,
            iterations: pbkdf2Iterations
        )
        let newSealed = try AES.GCM.seal(privRaw, using: newKEK, authenticating: VaultCryptoPrimitives.wrapAAD)
        guard let newCombined = newSealed.combined else {
            throw VaultError.cryptoFailure("Failed to re-wrap vault key.")
        }

        let updated = VaultManifest(
            version: manifest.version,
            pbkdf2Iterations: pbkdf2Iterations,
            pbkdf2SaltBase64: newSalt.base64EncodedString(),
            publicKeyBase64: manifest.publicKeyBase64,
            wrappedPrivateKeyBase64: newCombined.base64EncodedString(),
            createdAtISO8601: manifest.createdAtISO8601
        )
        try writeManifestAtomically(updated)
    }

    func resetVaultDeletingAllClips() throws {
        lock()
        if FileManager.default.fileExists(atPath: clipsDirectoryURL.path) {
            let items = (try? FileManager.default.contentsOfDirectory(at: clipsDirectoryURL, includingPropertiesForKeys: nil)) ?? []
            for item in items {
                try? secureDeleteClip(url: item)
            }
            try? FileManager.default.removeItem(at: clipsDirectoryURL)
        }
        if FileManager.default.fileExists(atPath: manifestURL.path) {
            try FileManager.default.removeItem(at: manifestURL)
        }
        ensureDirectories()
    }

    /// Creates a new encrypted clip writer using ONLY the public key from `vault_manifest.json`.
    /// Works even when the vault is locked and the Mac screen is locked!
    func createClipWriter(width: Int, height: Int, hasAudio: Bool) throws -> VaultClipFileWriter {
        ensureDirectories()
        let manifest = try loadManifest()
        guard let pubRaw = Data(base64Encoded: manifest.publicKeyBase64) else {
            throw VaultError.corruptedManifest
        }
        let vaultPublicKey = try Curve25519.KeyAgreement.PublicKey(rawRepresentation: pubRaw)

        let ephemeralPrivate = Curve25519.KeyAgreement.PrivateKey()
        let sharedSecret = try ephemeralPrivate.sharedSecretFromKeyAgreement(with: vaultPublicKey)
        let hkdfSalt = try VaultCryptoPrimitives.randomBytes(32)
        let clipUUID = UUID()
        let clipKey = VaultCryptoPrimitives.deriveClipKey(sharedSecret: sharedSecret, salt: hkdfSalt, clipUUID: clipUUID)

        let now = Date()
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        let stamp = formatter.string(from: now)
        let shortID = String(clipUUID.uuidString.prefix(8)).lowercased()
        let filename = "MaxWatch_\(stamp)_\(shortID).mwvault"
        let fileURL = clipsDirectoryURL.appendingPathComponent(filename, isDirectory: false)

        let flags: UInt8 = hasAudio ? 0x01 : 0x00
        let header = ClipFileHeader(
            version: ClipFileHeader.currentVersion,
            flags: flags,
            clipUUID: clipUUID,
            createdUnixMs: Int64((now.timeIntervalSince1970 * 1000.0).rounded()),
            width: UInt32(max(1, width)),
            height: UInt32(max(1, height)),
            ephemeralPublicKeyRaw: ephemeralPrivate.publicKey.rawRepresentation,
            hkdfSalt: hkdfSalt,
            durationMs: 0
        )

        return try VaultClipFileWriter(fileURL: fileURL, header: header, clipKey: clipKey)
    }

    func openClipReader(url: URL) throws -> VaultClipReader {
        let privKey = try copyUnlockedPrivateKey()
        let fd = url.path.withCString { Darwin.open($0, O_RDONLY | O_CLOEXEC) }
        guard fd >= 0 else {
            throw VaultError.ioFailure("Unable to open clip file.")
        }
        let headerData: Data
        do {
            headerData = try VaultClipReader.preadExact(fd: fd, offset: 0, length: ClipFileHeader.byteSize)
            Darwin.close(fd)
        } catch {
            Darwin.close(fd)
            throw error
        }
        let header = try ClipFileHeader.parse(from: headerData)
        let ephPub = try Curve25519.KeyAgreement.PublicKey(rawRepresentation: header.ephemeralPublicKeyRaw)
        let sharedSecret = try privKey.sharedSecretFromKeyAgreement(with: ephPub)
        let clipKey = VaultCryptoPrimitives.deriveClipKey(
            sharedSecret: sharedSecret,
            salt: header.hkdfSalt,
            clipUUID: header.clipUUID
        )
        return try VaultClipReader(fileURL: url, clipKey: clipKey)
    }

    func countVaultClips() -> Int {
        guard let urls = try? FileManager.default.contentsOfDirectory(
            at: clipsDirectoryURL,
            includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]
        ) else {
            return 0
        }
        return urls.filter { $0.pathExtension.lowercased() == "mwvault" }.count
    }

    func listSummaries(excludeActiveURL: URL? = nil) -> [EncryptedClipSummary] {
        guard let urls = try? FileManager.default.contentsOfDirectory(
            at: clipsDirectoryURL,
            includingPropertiesForKeys: [.fileSizeKey, .creationDateKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        var results: [EncryptedClipSummary] = []
        for url in urls where url.pathExtension.lowercased() == "mwvault" {
            if let active = excludeActiveURL, url.standardizedFileURL == active.standardizedFileURL {
                continue
            }
            let fd = url.path.withCString { Darwin.open($0, O_RDONLY | O_CLOEXEC) }
            guard fd >= 0 else { continue }
            var st = stat()
            guard Darwin.fstat(fd, &st) == 0, st.st_size >= off_t(ClipFileHeader.byteSize) else {
                Darwin.close(fd)
                continue
            }
            let fileSize = Int64(st.st_size)
            guard let headerData = try? VaultClipReader.preadExact(fd: fd, offset: 0, length: ClipFileHeader.byteSize),
                  let header = try? ClipFileHeader.parse(from: headerData) else {
                Darwin.close(fd)
                continue
            }

            // Quickly count chunk frames without decrypting
            var cursor = UInt64(ClipFileHeader.byteSize)
            var chunkCount = 0
            var mediaFragments = 0
            let uSize = UInt64(fileSize)
            while cursor + 5 <= uSize {
                guard let pref = try? VaultClipReader.preadExact(fd: fd, offset: cursor, length: 5),
                      pref.count == 5 else { break }
                var lenBE: UInt32 = 0
                _ = withUnsafeMutableBytes(of: &lenBE) { pref.copyBytes(to: $0, from: 0..<4) }
                let cLen = Int(UInt32(bigEndian: lenBE))
                guard cLen >= 28, cLen <= 64_000_000,
                      let kind = VaultSegmentKind(rawValue: pref[4]),
                      cursor + 5 + UInt64(cLen) <= uSize else {
                    break
                }
                chunkCount += 1
                if kind == .mediaFragment { mediaFragments += 1 }
                cursor += 5 + UInt64(cLen)
            }
            Darwin.close(fd)

            // Require at least init segment + 1 media segment to be playable
            guard chunkCount >= 2, mediaFragments >= 1 else { continue }

            let headerDur = Double(header.durationMs) / 1000.0
            let estDur = headerDur > 0.1 ? headerDur : Double(mediaFragments) * 1.0
            let created = Date(timeIntervalSince1970: Double(header.createdUnixMs) / 1000.0)

            results.append(EncryptedClipSummary(
                url: url,
                clipUUID: header.clipUUID,
                createdAt: created,
                fileSizeBytes: fileSize,
                width: Int(header.width),
                height: Int(header.height),
                hasAudio: header.hasAudio,
                cleanlyFinalized: header.cleanlyFinalized,
                headerDurationSeconds: headerDur,
                validChunkCount: chunkCount,
                estimatedDurationSeconds: estDur
            ))
        }

        results.sort { $0.createdAt < $1.createdAt }
        return results
    }

    func secureDeleteClip(url: URL) throws {
        let fd = url.path.withCString { Darwin.open($0, O_RDWR | O_CLOEXEC) }
        if fd >= 0 {
            var st = stat()
            if Darwin.fstat(fd, &st) == 0, st.st_size > 0 {
                let wipeBytes = Int(min(st.st_size, 131_072)) // Wipe header + init segment + first media keys
                let zeros = Data(count: wipeBytes)
                zeros.withUnsafeBytes { ptr in
                    if let base = ptr.baseAddress {
                        _ = Darwin.pwrite(fd, base, wipeBytes, 0)
                    }
                }
                _ = Darwin.fsync(fd)
            }
            Darwin.close(fd)
        }
        try FileManager.default.removeItem(at: url)
    }

    func exportClipToDirectory(clipURL: URL, directoryURL: URL) throws -> URL {
        let baseName = clipURL.deletingPathExtension().lastPathComponent
        var candidate = directoryURL.appendingPathComponent("\(baseName).mp4", isDirectory: false)
        var suffix = 2
        while FileManager.default.fileExists(atPath: candidate.path) {
            candidate = directoryURL.appendingPathComponent("\(baseName)-\(suffix).mp4", isDirectory: false)
            suffix += 1
        }
        try exportClipAsMP4(clipURL: clipURL, destinationURL: candidate)
        return candidate
    }

    func exportClipAsMP4(clipURL: URL, destinationURL: URL) throws {
        let reader = try openClipReader(url: clipURL)
        if FileManager.default.fileExists(atPath: destinationURL.path) {
            try FileManager.default.removeItem(at: destinationURL)
        }
        let outFD = destinationURL.path.withCString {
            Darwin.open($0, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
        }
        guard outFD >= 0 else {
            throw VaultError.ioFailure("Cannot create export file.")
        }
        defer { Darwin.close(outFD) }

        var offset: Int64 = 0
        let step = 1_048_576
        while offset < reader.totalPlaintextLength {
            var chunk = try reader.readPlaintextRange(offset: offset, length: step)
            defer { VaultCryptoPrimitives.secureZero(&chunk) }
            if chunk.isEmpty { break }
            try chunk.withUnsafeBytes { rawBuf in
                guard let base = rawBuf.baseAddress else { return }
                var written = 0
                while written < chunk.count {
                    let n = Darwin.write(outFD, base.advanced(by: written), chunk.count - written)
                    if n < 0 {
                        if errno == EINTR { continue }
                        throw VaultError.ioFailure("Export write failed")
                    }
                    written += n
                }
            }
            offset += Int64(chunk.count)
        }
        _ = Darwin.fsync(outFD)
    }

    private func copyUnlockedPrivateKey() throws -> Curve25519.KeyAgreement.PrivateKey {
        stateLock.lock()
        guard let raw = unlockedPrivateKeyBytes else {
            stateLock.unlock()
            throw VaultError.vaultLocked
        }
        let copy = Data(raw)
        stateLock.unlock()
        return try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: copy)
    }

    private func loadManifest() throws -> VaultManifest {
        guard FileManager.default.fileExists(atPath: manifestURL.path) else {
            throw VaultError.vaultNotInitialized
        }
        let data = try Data(contentsOf: manifestURL)
        do {
            return try JSONDecoder().decode(VaultManifest.self, from: data)
        } catch {
            throw VaultError.corruptedManifest
        }
    }

    private func writeManifestAtomically(_ manifest: VaultManifest) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(manifest)
        let tmpURL = vaultDirectoryURL.appendingPathComponent("vault_manifest.tmp", isDirectory: false)
        try data.write(to: tmpURL, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: NSNumber(value: Int16(0o600))], ofItemAtPath: tmpURL.path)
        if FileManager.default.fileExists(atPath: manifestURL.path) {
            _ = try FileManager.default.replaceItemAt(manifestURL, withItemAt: tmpURL)
        } else {
            try FileManager.default.moveItem(at: tmpURL, to: manifestURL)
        }
        try FileManager.default.setAttributes([.posixPermissions: NSNumber(value: Int16(0o600))], ofItemAtPath: manifestURL.path)
    }
}
