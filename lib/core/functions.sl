// Core declarations for language built-ins.
//
// Intrinsic behavior/signature details are defined by the compiler.
// See docs/library.md for the full call forms, especially for `new` and `sizeof`.
fn len(x &str) u32 {
	return x.len()
}

fn cstr(s &str) &u8 {
	return s.cstr()
}

fn concat(a &str, b &str) *str {
	return null as *str
}

pub struct FmtValue {
	kind u8
	repr *str
}

const CH_LBRACE u8 = '{'

const CH_RBRACE u8 = '}'

const CH_I u8 = 'i'

const CH_R u8 = 'r'

pub fn fmt(out *str, format &str, args ...FmtValue) u32 {
	var outBytes *[u8] = out
	var cap      u32   = len(out)
	var i        u32   = 0
	var ai       u32   = 0
	var oi       u32   = 0
	var bs       &[u8] = format

	for i < len(bs) {
		var ch = bs[i]

		if ch == CH_LBRACE {
			if i + 1 < len(bs) && bs[i + 1] == CH_LBRACE {
				oi = __fmt_write_byte(outBytes, cap, oi, ch: CH_LBRACE)
				i += 2
				continue
			}
			if i + 2 < len(bs) && bs[i + 2] == CH_RBRACE {
				var spec = bs[i + 1]
				if spec == CH_I || spec == CH_R {
					if ai >= len(args) {
						panic("invalid format string: placeholder index out of bounds")
						return 0
					}
					var v = args[ai]
					if spec == CH_I && v.kind != 1 && v.kind != 2 {
						panic("invalid format string: {i} requires integer argument")
						return 0
					}
					oi = __fmt_write_text(outBytes, cap, oi, text: v.repr)
					ai += 1
					i += 3
					continue
				}
			}
			panic("invalid format string")
			return 0
		}

		if ch == CH_RBRACE {
			if i + 1 < len(bs) && bs[i + 1] == CH_RBRACE {
				oi = __fmt_write_byte(outBytes, cap, oi, ch: CH_RBRACE)
				i += 2
				continue
			}
			panic("invalid format string")
			return 0
		}

		oi = __fmt_write_byte(outBytes, cap, oi, ch)
		i += 1
	}

	if ai != len(args) {
		panic("invalid format string: argument count mismatch")
		return 0
	}

	return oi
}

fn __fmt_write_byte(out *[u8], cap u32, oi u32, ch u8) u32 {
	if oi >= cap {
		panic("fmt output buffer too small")
		return 0
	}
	out[oi] = ch
	return oi + 1
}

fn __fmt_write_text(out *[u8], cap u32, oi u32, text &str) u32 {
	var b &[u8] = text
	for var i u32 = 0; i < len(b); i += 1 {
		oi = __fmt_write_byte(out, cap, oi, ch: b[i])
	}
	return oi
}

fn free() {}

fn panic(message &str) {}

pub fn print(message &str) context PrintContext {
	context.log.handler(&context.log, message, LogLevel.Info, 0 as LogFlags)
}

fn sizeof() uint {
	return 0
}
