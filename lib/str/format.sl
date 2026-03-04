pub fn fmt_write_byte(buf *[u8], payloadCap uint, oi uint, ch u8) uint {
	if oi < payloadCap {
		buf[oi] = ch
	}
	return oi + 1
}

pub fn fmt_write_text(buf *[u8], payloadCap uint, oi uint, text &str) uint {
	var b &[u8] = text
	for var i uint = 0; i < len(b); i += 1 {
		oi = fmt_write_byte(buf, payloadCap, oi, ch: b[i])
	}
	return oi
}

pub fn fmt_write_u64(buf *[u8], payloadCap uint, oi uint, x u64) uint {
	var v u64 = x
	if v == 0 as u64 {
		return fmt_write_byte(buf, payloadCap, oi, ch: '0' as u8)
	}

	var digits [u8 32]
	var n      uint = 0
	for v > 0 as u64 {
		var d u64 = v % 10
		digits[n] = '0' as u8 + d as u8
		n += 1
		v /= 10 as u64
	}
	for n > 0 {
		n -= 1
		oi = fmt_write_byte(buf, payloadCap, oi, ch: digits[n])
	}
	return oi
}

pub fn fmt_write_i64(buf *[u8], payloadCap uint, oi uint, x i64) uint {
	if x < 0 as i64 {
		oi = fmt_write_byte(buf, payloadCap, oi, ch: '-' as u8)
		if x == -9223372036854775808 as i64 {
			return fmt_write_u64(buf, payloadCap, oi, x: 9223372036854775808 as u64)
		}
		return fmt_write_u64(buf, payloadCap, oi, x: (-x) as u64)
	}
	return fmt_write_u64(buf, payloadCap, oi, x: x as u64)
}

pub fn fmt_write_f64(buf *[u8], payloadCap uint, oi uint, x f64) uint {
	var v f64 = x
	if v < 0 as f64 {
		oi = fmt_write_byte(buf, payloadCap, oi, ch: '-' as u8)
		v = -v
	}

	var whole i64 = v as i64
	oi = fmt_write_i64(buf, payloadCap, oi, x: whole)

	var frac f64 = v - whole as f64
	if frac <= 0 as f64 {
		return oi
	}

	var digits [u8 12]
	var n      uint = 0
	for n < 6 {
		frac *= 10 as f64
		var d i64 = frac as i64
		digits[n] = '0' as u8 + d as u8
		n += 1
		frac -= d as f64
		if frac < 0.000001 as f64 {
			break
		}
	}

	for n > 0 && digits[n - 1] == '0' as u8 {
		n -= 1
	}
	if n == 0 {
		return oi
	}

	oi = fmt_write_byte(buf, payloadCap, oi, ch: '.' as u8)
	for var i uint = 0; i < n; i += 1 {
		oi = fmt_write_byte(buf, payloadCap, oi, ch: digits[i])
	}
	return oi
}

pub fn fmt_write_i_any(buf *[u8], payloadCap uint, oi uint, v anytype) uint {
	const t = typeof(v)
	if t == i8 || t == i16 || t == i32 || t == i64 || t == int {
		return fmt_write_i64(buf, payloadCap, oi, x: v as i64)
	} else if t == u8 || t == u16 || t == u32 || t == u64 || t == uint {
		return fmt_write_u64(buf, payloadCap, oi, x: v as u64)
	}

	panic("invalid format string: {i} requires integer argument")
	return 0
}

pub fn fmt_write_f_any(buf *[u8], payloadCap uint, oi uint, v anytype) uint {
	const t = typeof(v)
	if t == f32 || t == f64 {
		return fmt_write_f64(buf, payloadCap, oi, x: v as f64)
	}

	panic("invalid format string: {f} requires floating-point argument")
	return 0
}

pub fn fmt_write_s_any(buf *[u8], payloadCap uint, oi uint, v anytype) uint {
	const t = typeof(v)
	if t == typeof("" as &str) {
		var s &str = v as &str
		return fmt_write_text(buf, payloadCap, oi, text: s)
	} else if t == typeof(null as *str) {
		var p *str = v as *str
		var s &str = p
		return fmt_write_text(buf, payloadCap, oi, text: s)
	}
	panic("invalid format string: {s} requires string argument")
	return 0
}

pub fn format(buf *[u8], const fmt &str, args ...anytype) uint {
	const {
		var bs &[u8] = fmt
		var i  uint  = 0
		var ai uint  = 0

		for i < len(bs) {
			var ch u8 = bs[i]
			if ch == '{' {
				if i + 1 < len(bs) && bs[i + 1] == '{' {
					i += 2
					continue
				}
				if i + 2 < len(bs) && bs[i + 2] == '}' {
					var spec u8 = bs[i + 1]

					assert ai < len(args)

					if spec == 'i' {
						if typeof(args[ai]) == i8 {} else if typeof(args[ai]) == i16 {} else if typeof(args[ai]) == i32 {} else if typeof(args[ai]) == i64 {} else if typeof(args[ai]) == int {} else if typeof(args[ai]) == u8 {} else if typeof(args[ai]) == u16 {} else if typeof(args[ai]) == u32 {} else if typeof(args[ai]) == u64 {} else if typeof(args[ai]) == uint {} else {
							assert false
						}
					} else if spec == 'f' {
						if typeof(args[ai]) == f32 {} else if typeof(args[ai]) == f64 {} else {
							assert false
						}
					} else if spec == 's' {
						if typeof(args[ai]) == typeof("" as &str) {} else {
							assert false
						}
					} else {
						assert false
					}

					ai += 1
					i += 3
					continue
				}
				assert false
			}

			if ch == '}' {
				if i + 1 < len(bs) && bs[i + 1] == '}' {
					i += 2
					continue
				}
				assert false
			}

			i += 1
		}

		assert ai == len(args)
	}

	var cap        uint = len(buf)
	var payloadCap uint = 0
	if cap > 0 {
		payloadCap = cap - 1
	}

	var bs &[u8] = fmt
	var i  uint  = 0
	var ai uint  = 0
	var oi uint  = 0

	for i < len(bs) {
		var ch u8 = bs[i]
		if ch == '{' {
			i += 1
			if i >= len(bs) {
				panic("invalid format string")
				return 0
			}

			var c1 u8 = bs[i]
			if c1 == '{' {
				oi = fmt_write_byte(buf, payloadCap, oi, ch: '{' as u8)
				i += 1
				continue
			}

			var spec u8 = c1
			i += 1
			if i >= len(bs) || bs[i] != '}' {
				panic("invalid format string")
				return 0
			}

			if ai >= len(args) {
				panic("invalid format string: placeholder index out of bounds")
				return 0
			}

			if spec == 'i' {
				oi = fmt_write_i_any(buf, payloadCap, oi, v: args[ai])
			} else if spec == 'f' {
				var t type = typeof(args[ai])
				if t == typeof(0 as f32) {
					oi = fmt_write_f64(buf, payloadCap, oi, x: args[ai] as f32 as f64)
				} else if t == typeof(0 as f64) {
					oi = fmt_write_f64(buf, payloadCap, oi, x: args[ai] as f64)
				} else {
					panic("invalid format string: {f} requires floating-point argument")
					return 0
				}
			} else if spec == 's' {
				var t type = typeof(args[ai])
				if t == typeof("" as &str) {
					var s &str = args[ai] as &str
					oi = fmt_write_text(buf, payloadCap, oi, text: s)
				} else if t == typeof(null as *str) {
					var p *str = args[ai] as *str
					var s &str = p
					oi = fmt_write_text(buf, payloadCap, oi, text: s)
				} else {
					panic("invalid format string: {s} requires string argument")
					return 0
				}
			} else {
				panic("invalid format string")
				return 0
			}
			ai += 1
			i += 1
			continue
		}

		if ch == '}' {
			i += 1
			if i < len(bs) && bs[i] == '}' {
				oi = fmt_write_byte(buf, payloadCap, oi, ch: '}' as u8)
				i += 1
				continue
			}
			panic("invalid format string")
			return 0
		}

		oi = fmt_write_byte(buf, payloadCap, oi, ch)
		i += 1
	}

	if cap > 0 {
		var nulPos uint = oi
		if nulPos > payloadCap {
			nulPos = payloadCap
		}
		buf[nulPos] = 0
	}

	return oi
}
