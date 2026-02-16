struct Packet {
    payloadLen u32
    sampleLen  u32
    payload    [.payloadLen]u8
    samples    [.sampleLen]i32
}

fn main() i32 {
    var p *Packet = 0 as *Packet
    p.payload = 0 as *u8
    return 0
}
