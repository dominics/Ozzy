// duplex: set a CoreAudio device's rate, play sines on outputs, record inputs, write a WAV.
// usage: duplex <name-substring> <rate> <seconds> <out.wav>
// out ch1 = 997 Hz, out ch3 = 1499 Hz, both -36 dBFS; ch2/ch4 silent.
import CoreAudio
import Foundation

func prop<T>(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ v: inout T) -> OSStatus {
    var a = AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    var sz = UInt32(MemoryLayout<T>.size)
    return AudioObjectGetPropertyData(obj, &a, 0, nil, &sz, &v)
}

let args = CommandLine.arguments
guard args.count == 5, let rate = Double(args[2]), let secs = Double(args[3]) else {
    FileHandle.standardError.write("usage: duplex <name> <rate> <seconds> <out.wav>\n".data(using: .utf8)!); exit(2)
}

var a = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
var sz: UInt32 = 0
AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &sz)
var ids = [AudioObjectID](repeating: 0, count: Int(sz) / 4)
AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &sz, &ids)
var dev: AudioObjectID = 0
for id in ids {
    var name: CFString = "" as CFString
    _ = prop(id, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &name)
    if (name as String).contains(args[1]) { dev = id; break }
}
guard dev != 0 else { print("device not found"); exit(1) }

func channels(_ scope: AudioObjectPropertyScope) -> Int {
    var a = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    var sz: UInt32 = 0
    AudioObjectGetPropertyDataSize(dev, &a, 0, nil, &sz)
    let p = UnsafeMutableRawPointer.allocate(byteCount: Int(sz), alignment: 16); defer { p.deallocate() }
    AudioObjectGetPropertyData(dev, &a, 0, nil, &sz, p)
    let l = UnsafeMutableAudioBufferListPointer(p.assumingMemoryBound(to: AudioBufferList.self))
    return l.reduce(0) { $0 + Int($1.mNumberChannels) }
}
let nIn = channels(kAudioObjectPropertyScopeInput), nOut = channels(kAudioObjectPropertyScopeOutput)

var cur: Float64 = 0
_ = prop(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &cur)
if cur != rate {
    var r = rate
    var ra = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    let st = AudioObjectSetPropertyData(dev, &ra, 0, nil, UInt32(MemoryLayout<Float64>.size), &r)
    if st != noErr { print("set rate failed: \(st)"); exit(1) }
    for _ in 0..<50 {
        Thread.sleep(forTimeInterval: 0.1)
        _ = prop(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &cur)
        if cur == rate { break }
    }
}
print("device \(dev) rate \(cur) in \(nIn) out \(nOut)")
guard cur == rate else { print("rate did not change"); exit(1) }
Thread.sleep(forTimeInterval: 1.0)

let total = Int(rate * secs)
let rec = UnsafeMutablePointer<Float>.allocate(capacity: total * nIn); rec.initialize(repeating: 0, count: total * nIn)
var written = 0
var phase1 = 0.0, phase3 = 0.0
let amp = Float(pow(10.0, -36.0 / 20.0))
var cycles = 0, overloads = 0

var a2 = AudioObjectPropertyAddress(mSelector: kAudioDeviceProcessorOverload, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
AudioObjectAddPropertyListenerBlock(dev, &a2, nil) { _, _ in overloads += 1 }

var procID: AudioDeviceIOProcID?
AudioDeviceCreateIOProcIDWithBlock(&procID, dev, nil) { _, inData, _, outData, _ in
    cycles += 1
    let ins = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inData))
    // inputs: assume interleaved single buffer or one buffer per channel
    var ch = 0
    var frames = 0
    for b in ins {
        let nc = Int(b.mNumberChannels)
        guard let d = b.mData?.assumingMemoryBound(to: Float.self) else { ch += nc; continue }
        frames = Int(b.mDataByteSize) / (4 * nc)
        for f in 0..<frames where written + f < total {
            for c in 0..<nc { rec[(written + f) * nIn + ch + c] = d[f * nc + c] }
        }
        ch += nc
    }
    let outs = UnsafeMutableAudioBufferListPointer(outData)
    var oc = 0
    var oframes = 0
    for b in outs {
        let nc = Int(b.mNumberChannels)
        guard let d = b.mData?.assumingMemoryBound(to: Float.self) else { oc += nc; continue }
        oframes = Int(b.mDataByteSize) / (4 * nc)
        for f in 0..<oframes {
            for c in 0..<nc {
                let g = oc + c
                var s: Float = 0
                if g == 0 { s = amp * Float(sin(phase1 + 2 * .pi * 997 * Double(f) / rate)) }
                if g == 2 { s = amp * Float(sin(phase3 + 2 * .pi * 1499 * Double(f) / rate)) }
                d[f * nc + c] = s
            }
        }
        oc += nc
    }
    phase1 = fmod(phase1 + 2 * .pi * 997 * Double(oframes) / rate, 2 * .pi)
    phase3 = fmod(phase3 + 2 * .pi * 1499 * Double(oframes) / rate, 2 * .pi)
    written += frames
}
AudioDeviceStart(dev, procID)
while written < total { Thread.sleep(forTimeInterval: 0.05) }
AudioDeviceStop(dev, procID)
AudioDeviceDestroyIOProcID(dev, procID!)
print("cycles \(cycles) overloads \(overloads) frames \(written)")

// write 32-bit float WAV
var data = Data()
func u32(_ v: UInt32) { var x = v.littleEndian; data.append(Data(bytes: &x, count: 4)) }
func u16(_ v: UInt16) { var x = v.littleEndian; data.append(Data(bytes: &x, count: 2)) }
let bytes = UInt32(total * nIn * 4)
data.append("RIFF".data(using: .ascii)!); u32(36 + bytes); data.append("WAVEfmt ".data(using: .ascii)!)
u32(16); u16(3); u16(UInt16(nIn)); u32(UInt32(rate)); u32(UInt32(rate) * UInt32(nIn) * 4); u16(UInt16(nIn * 4)); u16(32)
data.append("data".data(using: .ascii)!); u32(bytes)
data.append(Data(bytes: rec, count: Int(bytes)))
try! data.write(to: URL(fileURLWithPath: args[4]))
