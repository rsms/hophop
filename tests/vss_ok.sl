struct Packet {
    payloadLen u32
    sampleLen  u32
    payload    [.payloadLen]u8
    samples    [.sampleLen]i32
}

fn main() i32 {
    var p *Packet = 0 as *Packet
    var payload *u8 = p.payload
    var payloadPtr **u8 = &p.payload
    payload = *payloadPtr

    var samples *i32 = p.samples
    var samplesPtr **i32 = &p.samples
    samples = *samplesPtr
    return 0
}
