import AVFoundation
import Foundation
import UniformTypeIdentifiers

// MARK: - Zero-Disk-Plaintext AVAssetResourceLoaderDelegate

final class VaultAssetResourceLoader: NSObject, AVAssetResourceLoaderDelegate {
    static let customScheme = "mwvault"
    let loaderQueue = DispatchQueue(label: "app.c60.maxwatch.vaultResourceLoader", qos: .userInteractive)
    private let reader: VaultClipReader

    init(reader: VaultClipReader) {
        self.reader = reader
        super.init()
    }

    static func makePlayableAsset(reader: VaultClipReader) -> (AVURLAsset, VaultAssetResourceLoader) {
        let loader = VaultAssetResourceLoader(reader: reader)
        let url = URL(string: "\(customScheme)://clip/\(reader.header.clipUUID.uuidString).mp4")!
        let asset = AVURLAsset(url: url)
        asset.resourceLoader.setDelegate(loader, queue: loader.loaderQueue)
        return (asset, loader)
    }

    func resourceLoader(
        _ resourceLoader: AVAssetResourceLoader,
        shouldWaitForLoadingOfRequestedResource loadingRequest: AVAssetResourceLoadingRequest
    ) -> Bool {
        if let infoReq = loadingRequest.contentInformationRequest {
            infoReq.contentType = UTType.mpeg4Movie.identifier
            infoReq.contentLength = reader.totalPlaintextLength
            infoReq.isByteRangeAccessSupported = true
        }

        if let dataReq = loadingRequest.dataRequest {
            let startOffset = dataReq.currentOffset > 0 ? dataReq.currentOffset : dataReq.requestedOffset
            let remainingFromStart = max(0, reader.totalPlaintextLength - startOffset)
            let lengthToServe: Int
            if dataReq.requestsAllDataToEndOfResource {
                lengthToServe = Int(min(Int64(Int.max), remainingFromStart))
            } else {
                lengthToServe = Int(min(Int64(dataReq.requestedLength), remainingFromStart))
            }

            do {
                // Stream in 512 KB slices so huge requests stay responsive and low-memory
                var served = 0
                let sliceSize = 524_288
                while served < lengthToServe && !loadingRequest.isCancelled {
                    let n = min(sliceSize, lengthToServe - served)
                    let chunk = try reader.readPlaintextRange(offset: startOffset + Int64(served), length: n)
                    if chunk.isEmpty { break }
                    dataReq.respond(with: chunk)
                    served += chunk.count
                }
                loadingRequest.finishLoading()
            } catch {
                loadingRequest.finishLoading(with: error)
            }
            return true
        }

        loadingRequest.finishLoading()
        return true
    }
}
