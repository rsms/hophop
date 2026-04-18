// Verifies new VSS missing initialization is rejected.
struct Packet {
	payloadLen u32
	sampleLen  u32
	name       str
	payload    [u8 .payloadLen]
	samples    [i32 .sampleLen]
}

fn main() {
	var ma             = context.mem
	var packet *Packet = new Packet with ma
	_ = packet
	_ = ma
}
