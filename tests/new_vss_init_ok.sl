// Verifies new VSS initialization is accepted.
struct Packet {
	payloadLen u32
	sampleLen  u32
	name       str
	payload    [u8 .payloadLen]
	samples    [i32 .sampleLen]
}

struct PlainStruct {
	name &str
	x    uint
	y    uint
}

fn main() {
	var packet *Packet = new Packet{ payloadLen: 3, sampleLen: 2, name.len: 4 }
	assert packet.payloadLen == 3
	assert packet.sampleLen == 2
	assert packet.name.len == 4
	assert packet.payload[0] == 0
	assert packet.samples[0] == 0

	var plain *PlainStruct = new PlainStruct{ name: "Monz", y: 2 }
	assert plain.name == "Monz"
	assert plain.x == 0
	assert plain.y == 2

	var string *str = new str{ len: 5 }
	assert string.len == 5
}
