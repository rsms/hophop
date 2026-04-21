import "platform/playbit" { clock_monotonic }

pub const Nanosecond i64 = 1

pub const Microsecond i64 = 1000

pub const Millisecond i64 = 1000 * 1000

pub const Second i64 = 1000 * 1000 * 1000

pub const Minute i64 = 60 * Second

pub const Hour i64 = 60 * Minute

pub fn now() u64 {
	return clock_monotonic()
}

pub fn since(past u64) i64 {
	return now() as i64 - past as i64
}

pub fn until(future u64) i64 {
	return future as i64 - now() as i64
}
