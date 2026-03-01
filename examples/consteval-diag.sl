// consteval diagnostics from pure SL code
import "compiler"
import "reflect"

fn require_non_zero(value i64, span &reflect.Span) i64 {
	if value == 0 as i64 {
		compiler.error_at(span, "value must be non-zero")
	}
	return value
}

fn warn_if_small(value i64, span &reflect.Span) {
	if value < 16 as i64 {
		compiler.warn_at(span, "value is unusually small")
	}
}

const BUFFER_SIZE i64 = checked_buffer_size()

fn checked_buffer_size() i64 {
	const candidate i64 = 64
	warn_if_small(candidate, reflect.span_of(candidate))
	return require_non_zero(candidate, reflect.span_of(BUFFER_SIZE))
}

// Uncomment to see a compile-time error anchored to BAD_BUFFER_SIZE.
// const BAD_BUFFER_SIZE i64 = require_non_zero(0 as i64, reflect.span_of(BAD_BUFFER_SIZE))
fn main() {
	assert BUFFER_SIZE == 64 as i64
}
