// Verifies VSS bad assign.
struct Packet {
	payloadLen u32
	sampleLen  u32
	payload    [u8 .payloadLen]
	samples    [i32 .sampleLen]
}

fn main() i32 {
	var p    *Packet = (null as rawptr) as *Packet
	var none *u8     = (null as rawptr) as *u8
	p.payload = none
	return 0
}
