// The loopback tunnel the JIT helper reaches the device's own services
// through (app/JITHelper): iOS serves them to a paired computer over the
// network, never to an app on the device itself. Packets the helper sends
// to the peer address come into this tunnel, and go back out of it as if the
// peer had sent them to the device; the device's answers go back the same
// way. The idea of SideStore's StosVPN, later LocalDevVPN, which StikJIT
// expects at the same addresses by default.
//
// Started by the app for as long as the helper needs it (app/BuiltinJIT.m),
// with a route only to the peer: the device's other traffic stays out of it.

import NetworkExtension

final class PacketTunnelProvider: NEPacketTunnelProvider {
    // The device's address in the tunnel, and the peer's, which the helper
    // connects to (StikJIT.Configuration's default deviceAddress)
    private static let device: [UInt8] = [10, 7, 0, 0]
    private static let peer: [UInt8] = [10, 7, 0, 1]

    override func startTunnel(options: [String: NSObject]?, completionHandler: @escaping (Error?) -> Void) {
        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: "10.7.0.0")
        // /32s: a wider mask would take the routes of the network the device
        // is on into the tunnel
        let ipv4 = NEIPv4Settings(addresses: ["10.7.0.0"], subnetMasks: ["255.255.255.255"])
        ipv4.includedRoutes = [NEIPv4Route(destinationAddress: "10.7.0.1", subnetMask: "255.255.255.255")]
        settings.ipv4Settings = ipv4
        setTunnelNetworkSettings(settings) { error in
            if error == nil {
                self.reflect()
            }
            completionHandler(error)
        }
    }

    override func stopTunnel(with reason: NEProviderStopReason, completionHandler: @escaping () -> Void) {
        completionHandler()
    }

    // An IPv4 packet from the device to the peer comes back from the peer to
    // the device, and the device's answer, to the peer, the same. The two
    // addresses only change places, so neither the IP header's checksum nor
    // TCP's and UDP's (which cover them) changes.
    private func reflect() {
        packetFlow.readPackets { packets, protocols in
            var reflected = packets
            for i in reflected.indices where protocols[i].int32Value == AF_INET && reflected[i].count >= 20 {
                reflected[i].withUnsafeMutableBytes { (header: UnsafeMutableRawBufferPointer) in
                    let source = Array(header[12..<16]), destination = Array(header[16..<20])
                    if source == Self.device {
                        header.copyBytes(from: Self.peer, at: 12)
                    }
                    if destination == Self.peer {
                        header.copyBytes(from: Self.device, at: 16)
                    }
                }
            }
            self.packetFlow.writePackets(reflected, withProtocols: protocols)
            self.reflect()
        }
    }
}

private extension UnsafeMutableRawBufferPointer {
    func copyBytes(from bytes: [UInt8], at offset: Int) {
        UnsafeMutableRawBufferPointer(rebasing: self[offset..<offset + bytes.count]).copyBytes(from: bytes)
    }
}
