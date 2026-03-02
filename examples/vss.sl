// variable-size structs plus new allocation forms:
// - `new T{...}` for initialized allocation
// - VSS allocations require initializer (e.g. `new Packet{...}`)
// - for non-VSS structs, `new T` is equivalent to `new T{}`
struct Packet {
	payloadLen u32
	sampleLen  u32
	name       str
	payload    [u8 .payloadLen]
	samples    [i32 .sampleLen]
}

struct PlainStruct {
	name &str = "anon"
	x    uint = 7
	y    uint = x + 2
}

fn main() {
	var ma = context.mem

	// VSS: initializer is required so runtime allocation size can be computed.
	var packet *Packet = new Packet{ payloadLen: 3, sampleLen: 2, name.len: 4 } with ma
	assert packet.payloadLen == 3
	assert packet.sampleLen == 2
	assert packet.name.len == 4
	assert packet.payload[0] == 0
	assert packet.samples[0] == 0

	// This would be an error:
	// var bad *Packet = new Packet with ma
	// non-VSS: `new T` is equivalent to `new T{}`
	var a *PlainStruct = new PlainStruct with ma
	var b *PlainStruct = new PlainStruct{} with ma
	assert a.name == "anon"
	assert a.x == 7
	assert a.y == 9
	assert b.name == "anon"
	assert b.x == 7
	assert b.y == 9

	var c *PlainStruct = new PlainStruct{ name: "Monz", y: 2 } with ma
	assert c.name == "Monz"
	assert c.x == 7
	assert c.y == 2
}
