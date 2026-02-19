// variable-size structs with dependent trailing array fields

struct Packet {
    payloadLen u32
    sampleLen  u32
    payload    [u8 .payloadLen]
    samples    [i32 .sampleLen]
}

fn main() {
    var p = 0 as *Packet
    var payload = p.payload
    var payloadPtr = &p.payload
    payload = *payloadPtr

    var samples = p.samples
    var samplesPtr = &p.samples
    samples = *samplesPtr
}
