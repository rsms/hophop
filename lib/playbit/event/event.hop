import "platform/playbit" { event_poll }

pub const BufferSizeMax uint = 16384

pub fn poll(buffer *[u8], deadline u64, leeway i64) i32 {
	var leewayU64 = leeway as u64
	return event_poll(buffer, deadline, leeway: leewayU64)
}
