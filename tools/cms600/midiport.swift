// midiport: list CoreMIDI endpoints, or loop/listen on the source and destination whose names contain a substring.
// usage: midiport list | midiport loop <name> | midiport sysex <name> | midiport listen <name> [secs]
import CoreMIDI
import Foundation

func name(_ e: MIDIEndpointRef) -> String {
    var s: Unmanaged<CFString>?
    MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s)
    return (s?.takeRetainedValue() as String?) ?? "?"
}
func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02x", $0) }.joined(separator: " ") }

let args = CommandLine.arguments
guard args.count >= 2 else { print("usage: midiport list | loop <name> | sysex <name> | listen <name> [secs]"); exit(2) }
if args[1] == "list" {
    for i in 0..<MIDIGetNumberOfSources() { print("source \(i): \(name(MIDIGetSource(i)))") }
    for i in 0..<MIDIGetNumberOfDestinations() { print("destination \(i): \(name(MIDIGetDestination(i)))") }
    exit(0)
}
guard args.count >= 3 else { print("missing endpoint name"); exit(2) }
let want = args[2]
var src: MIDIEndpointRef = 0, dst: MIDIEndpointRef = 0
for i in 0..<MIDIGetNumberOfSources() where name(MIDIGetSource(i)).contains(want) { src = MIDIGetSource(i) }
for i in 0..<MIDIGetNumberOfDestinations() where name(MIDIGetDestination(i)).contains(want) { dst = MIDIGetDestination(i) }
guard src != 0, dst != 0 else { print("no source/destination containing '\(want)'"); exit(1) }

var received = [UInt8]()
let lock = NSLock()
var client = MIDIClientRef()
MIDIClientCreate("midiport" as CFString, nil, nil, &client)
var inPort = MIDIPortRef()
MIDIInputPortCreateWithBlock(client, "in" as CFString, &inPort) { list, _ in
    var p = list.pointee.packet
    for _ in 0..<list.pointee.numPackets {
        let bytes = withUnsafeBytes(of: p.data) { Array($0.prefix(Int(p.length))) }
        lock.lock(); received += bytes; lock.unlock()
        p = MIDIPacketNext(&p).pointee
    }
}
MIDIPortConnectSource(inPort, src, nil)
var outPort = MIDIPortRef()
MIDIOutputPortCreate(client, "out" as CFString, &outPort)

if args[1] == "listen" {
    let secs = Double(args.count > 3 ? args[3] : "20")!
    Thread.sleep(forTimeInterval: secs)
    lock.lock(); print("received \(received.count) bytes: \(hex(received))"); lock.unlock()
    exit(0)
}

var sent = [UInt8]()
func send(_ b: [UInt8]) {
    var buf = [UInt8](repeating: 0, count: 1024)
    buf.withUnsafeMutableBytes { raw in
        let pl = raw.baseAddress!.assumingMemoryBound(to: MIDIPacketList.self)
        var pkt = MIDIPacketListInit(pl)
        pkt = MIDIPacketListAdd(pl, 1024, pkt, 0, b.count, b)
        MIDISend(outPort, dst, pl)
    }
    sent += b
}
Thread.sleep(forTimeInterval: 0.5)
if args[1] == "sysex" {
    // Non-commercial SysEx only (0x7D), safe on SystemCtrl, which drives the FX processors.
    for len in [1, 2, 3, 4, 5, 7, 8, 13, 24, 100, 250] {
        send([0xF0, 0x7D] + (0..<len).map { UInt8(($0 + len) & 0x7F) } + [0xF7])
        Thread.sleep(forTimeInterval: 0.02)
    }
    for _ in 0..<32 { send([0xF0, 0x7D, 0x11, 0x22, 0x33, 0xF7]) }   // burst, no gaps
} else {
for n in 0..<32 { send([0x90, UInt8(36 + n), 100]); Thread.sleep(forTimeInterval: 0.01); send([0x80, UInt8(36 + n), 0]) }
for v in stride(from: 0, to: 128, by: 8) { send([0xB0, 7, UInt8(v)]) }
send([0xE0, 0x00, 0x40]); send([0xC0, 5])
send([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7])                          // identity request
send([0xF0, 0x7D] + (0..<100).map { UInt8($0 & 0x7F) } + [0xF7])   // 103-byte SysEx
for _ in 0..<64 { send([0x90, 60, 64]); send([0x80, 60, 0]) }       // burst, no gaps
}
Thread.sleep(forTimeInterval: 3.0)
lock.lock(); let r = received; lock.unlock()
print("sent \(sent.count) bytes, received \(r.count) bytes, identical: \(r == sent)")
if r != sent {
    let n = min(r.count, sent.count)
    if let i = (0..<n).first(where: { r[$0] != sent[$0] }) {
        print("first difference at byte \(i): sent \(String(format: "%02x", sent[i])) got \(String(format: "%02x", r[i]))")
    }
    print("received: \(hex(r))")
}
