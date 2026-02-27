struct Packet {
	payloadLen u32
	sampleLen  u32
	payload    [u8 .payloadLen]
	samples    [i32 .sampleLen]
}

fn main() i32 {
	var p Packet
	p.payloadLen = 1
	return p.payloadLen as i32
}
