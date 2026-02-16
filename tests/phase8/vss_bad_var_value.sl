struct Packet {
    payloadLen u32
    sampleLen  u32
    payload    [.payloadLen]u8
    samples    [.sampleLen]i32
}

fn main() i32 {
    var p Packet
    p.payloadLen = 1
    return p.payloadLen as i32
}
