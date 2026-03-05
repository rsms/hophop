fn fmt_write_byte(buf *[u8], payloadCap, oi uint, ch u8) uint {
	if oi < payloadCap {
		buf[oi] = ch
	}
	return oi + 1
}

fn fmt_write_text(buf *[u8], payloadCap, oi uint, text &str) uint {
	var b = text
	for var i uint = 0; i < len(b); i += 1 {
		oi = fmt_write_byte(buf, payloadCap, oi, ch: b[i])
	}
	return oi
}

fn fmt_write_u64(buf *[u8], payloadCap, oi uint, x u64) uint {
	var v = x
	if v == 0 {
		return fmt_write_byte(buf, payloadCap, oi, ch: '0')
	}

	var digits [u8 32]
	var n      uint = 0
	for v > 0 {
		var d = v % 10
		digits[n] = '0' + d as u8
		n += 1
		v /= 10
	}
	for n > 0 {
		n -= 1
		oi = fmt_write_byte(buf, payloadCap, oi, ch: digits[n])
	}
	return oi
}

fn fmt_write_i64(buf *[u8], payloadCap, oi uint, x i64) uint {
	if x < 0 {
		oi = fmt_write_byte(buf, payloadCap, oi, ch: '-')
		if x == -9223372036854775808 {
			return fmt_write_u64(buf, payloadCap, oi, x: 9223372036854775808 as u64)
		}
		return fmt_write_u64(buf, payloadCap, oi, x: (-x) as u64)
	}
	return fmt_write_u64(buf, payloadCap, oi, x: x as u64)
}

fn fmt_write_f64(buf *[u8], payloadCap, oi uint, x f64) uint {
	var v = x
	if v < 0 {
		oi = fmt_write_byte(buf, payloadCap, oi, ch: '-')
		v = -v
	}

	var whole = v as i64
	oi = fmt_write_i64(buf, payloadCap, oi, x: whole)

	var frac = v - whole as f64
	if frac <= 0 {
		return oi
	}

	var digits [u8 12]
	var n      uint = 0
	for n < 6 {
		frac *= 10
		var d = frac as i64
		digits[n] = '0' + d as u8
		n += 1
		frac -= d as f64
		if frac < 0.000001 {
			break
		}
	}

	for n > 0 && digits[n - 1] == '0' {
		n -= 1
	}
	if n == 0 {
		return oi
	}

	oi = fmt_write_byte(buf, payloadCap, oi, ch: '.')
	for var i uint = 0; i < n; i += 1 {
		oi = fmt_write_byte(buf, payloadCap, oi, ch: digits[i])
	}
	return oi
}

fn fmt_write_i_any(buf *[u8], payloadCap, oi uint, v anytype) uint {
	const t = typeof(v)
	if t == i8 || t == i16 || t == i32 || t == i64 || t == int {
		return fmt_write_i64(buf, payloadCap, oi, x: v as i64)
	} else if t == u8 || t == u16 || t == u32 || t == u64 || t == uint {
		return fmt_write_u64(buf, payloadCap, oi, x: v as u64)
	}

	panic("invalid format string: {i} requires integer argument")
	return 0
}

fn fmt_write_f_any(buf *[u8], payloadCap, oi uint, v anytype) uint {
	const t = typeof(v)
	if t == f32 || t == f64 {
		return fmt_write_f64(buf, payloadCap, oi, x: v as f64)
	}

	panic("invalid format string: {f} requires floating-point argument")
	return 0
}

fn fmt_write_s_any(buf *[u8], payloadCap, oi uint, v anytype) uint {
	const t = typeof(v)
	if t == typeof("" as &str) {
		var text = v as &str
		return fmt_write_text(buf, payloadCap, oi, text)
	} else if t == typeof(null as *str) {
		var text = v as *str
		return fmt_write_text(buf, payloadCap, oi, text)
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

	var cap             = len(buf)
	var payloadCap uint = 0
	if cap > 0 {
		payloadCap = cap - 1
	}

	var bs &[u8] = fmt
	var i  uint  = 0
	var ai uint  = 0
	var oi uint  = 0

	for i < len(bs) {
		var ch = bs[i]
		if ch == '{' {
			i += 1
			if i >= len(bs) {
				panic("invalid format string")
				return 0
			}

			var c1 = bs[i]
			if c1 == '{' {
				oi = fmt_write_byte(buf, payloadCap, oi, ch: '{')
				i += 1
				continue
			}

			var spec = c1
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
				var t = typeof(args[ai])
				if t == typeof(0 as f32) {
					oi = fmt_write_f64(buf, payloadCap, oi, x: args[ai] as f32 as f64)
				} else if t == typeof(0 as f64) {
					oi = fmt_write_f64(buf, payloadCap, oi, x: args[ai] as f64)
				} else {
					panic("invalid format string: {f} requires floating-point argument")
					return 0
				}
			} else if spec == 's' {
				var t = typeof(args[ai])
				if t == typeof("" as &str) {
					var text = args[ai] as &str
					oi = fmt_write_text(buf, payloadCap, oi, text)
				} else if t == typeof(null as *str) {
					var text = args[ai] as *str
					oi = fmt_write_text(buf, payloadCap, oi, text)
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
				oi = fmt_write_byte(buf, payloadCap, oi, ch: '}')
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
		var nulPos = oi
		if nulPos > payloadCap {
			nulPos = payloadCap
		}
		buf[nulPos] = 0
	}

	return oi
}
