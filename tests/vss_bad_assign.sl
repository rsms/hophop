// Verifies VSS bad assign.
struct Packet {
	payloadLen u32
	sampleLen  u32
	payload    [u8 .payloadLen]
	samples    [i32 .sampleLen]
}

fn main() i32 {
	var p *Packet = null as *Packet
	p.payload = null as *u8
	return 0
}
