// iSH's JIT helper: the debugger side of the JIT, in a process of its own.
//
// iOS runs code iSH writes only once a debugger has had iSH (CS_DEBUGGED),
// and from iOS 26 on TXM devices only in a region the debugger prepared. A
// process cannot be its own debugger: the brk iSH asks with stops all its
// threads, the one that would answer too. So the app starts this extension
// (app/BuiltinJIT.m, by NSExtension), and it attaches to the app through the
// device's own services, reached over a loopback tunnel, with StikJIT
// (app/StikJIT/build.sh), which answers iSH's requests with its universal
// script (asbestos/guest-arm64/jit.c, "JIT26").
//
// The request and its reply are an NSExtensionItem's userInfo, as
// BuiltinJIT.m makes and reads them:
//   request: "operation" "enable" | "prepare" | "resetDDI"; for "enable" and
//            "prepare" "pairingFile" (the pairing file's bytes: nothing of the
//            app's container is this process's to read); for "enable" "pid"
//            (the app's) and "forceScript" (iOS 26: the script attaches,
//            TXM or not, as iSH asks for its region by the protocol)
//   reply:   "outcome" "succeeded" | "failed", "detail" (one line), "log"
//            (StikJIT's progress, line by line), "txm" (if known)

import Foundation
import StikJIT

final class JITHelperRequestHandler: NSObject, NSExtensionRequestHandling {
    // StikJIT's calls block, and are to run on one serial queue
    private static let work = DispatchQueue(label: "app.ish.JITHelper")

    func beginRequest(with context: NSExtensionContext) {
        let request = (context.inputItems.first as! NSExtensionItem).userInfo!
        Self.work.async {
            var log: [String] = []
            var reply: [String: Any] = ["outcome": "failed"]
            if let txm = StikJIT.isTXMPresent {
                reply["txm"] = txm
            }
            do {
                reply["detail"] = try Self.perform(request) { log.append($0) }
                reply["outcome"] = "succeeded"
            } catch {
                reply["detail"] = error.localizedDescription
            }
            reply["log"] = log
            let item = NSExtensionItem()
            item.userInfo = reply
            context.completeRequest(returningItems: [item], completionHandler: nil)
        }
    }

    // The developer disk image, downloaded once and mounted once a boot: kept
    // in this extension's own Library
    private static var ddiPaths: DDIPaths {
        let library = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
        return DDIPaths.default(in: library.appendingPathComponent("StikJIT"))
    }

    private static func perform(_ request: [AnyHashable: Any], log: @escaping (String) -> Void) throws -> String {
        let operation = request["operation"] as! String
        switch operation {
        case "enable":
            let pid = request["pid"] as! Int
            let forceScript = request["forceScript"] as! Bool
            try withPairingFile(request["pairingFile"] as! Data) { pairingFile in
                try StikJIT.enableJIT(targetPID: Int32(pid), pairingFile: pairingFile, ddiPaths: ddiPaths,
                                      script: .universal, forceScript: forceScript,
                                      preparationProgress: { log(describe($0)) }, progress: log)
            }
            return "attached to iSH, its JIT requests answered, detached"
        case "prepare":
            let readiness = try withPairingFile(request["pairingFile"] as! Data) { pairingFile in
                StikJIT.prepareDevice(pairingFile: pairingFile, paths: ddiPaths) { log(describe($0)) }
            }
            switch readiness {
            case .ready:
                return "ready: the device's services reached, the developer disk image mounted"
            case .unreachable(let reason):
                throw HelperError("unreachable: \(reason)")
            case .preparationFailed(let reason):
                throw HelperError("not ready: \(reason)")
            @unknown default:
                // a StikJIT newer than this: what it said, for a case of its own
                throw HelperError("not ready: StikJIT said \(readiness), which this does not know")
            }
        case "resetDDI":
            try StikJIT.resetCachedDDI(at: ddiPaths)
            return "the developer disk image's cached files removed"
        default:
            throw HelperError("no operation \(operation)")
        }
    }

    // The pairing file as a file of this process's, for the length of body
    private static func withPairingFile<T>(_ data: Data, _ body: (URL) throws -> T) throws -> T {
        let url = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("pairing-\(getpid()).plist")
        try data.write(to: url, options: .atomic)
        defer { try? FileManager.default.removeItem(at: url) }
        return try body(url)
    }

    private static func describe(_ stage: StikJIT.PreparationStage) -> String {
        switch stage {
        case .checkingReachability:
            return "reaching the device's services"
        case .checkingDDI:
            return "checking the developer disk image"
        case .downloadingDDI(let fraction, let status):
            return String(format: "downloading the developer disk image: %.0f%% %@", fraction * 100, status)
        case .mountingDDI(let fraction):
            return String(format: "mounting the developer disk image: %.0f%%", fraction * 100)
        case .verifyingDDI:
            return "verifying the developer disk image"
        case .ready:
            return "the device is ready"
        @unknown default:
            // a StikJIT newer than this: its stage as it says it (a log line only)
            return "\(stage)"
        }
    }
}

private struct HelperError: LocalizedError {
    let errorDescription: String?
    init(_ description: String) {
        errorDescription = description
    }
}
