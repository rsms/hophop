struct Packet {
    payloadLen u32
    sampleLen  u32
    payload    [.payloadLen]u8
    samples    [.sampleLen]i32
}

fn main() i32 {
    var s usize = sizeof(Packet)
    return s as i32
}
