// Example: variable-size structs with dependent trailing array fields.

struct Packet {
    payloadLen u32
    sampleLen  u32
    payload    [u8 .payloadLen]
    samples    [i32 .sampleLen]
}

fn main() {
    var p *Packet = 0 as *Packet
    var payload *u8 = p.payload
    var payloadPtr &*u8 = &p.payload
    payload = *payloadPtr

    var samples *i32 = p.samples
    var samplesPtr &*i32 = &p.samples
    samples = *samplesPtr
}
