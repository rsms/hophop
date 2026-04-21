// Verifies const-eval const block call span of is accepted.
import "compiler"
import "reflect"

fn require_non_zero(const value i64, const span reflect.Span) i64 {
	const {
		if value == 0 {
			compiler.error_at(span, "value must be non-zero")
		}
	}
	return value
}

fn warn_if_small(const value i64, const span reflect.Span) {
	const {
		if value < 16 {
			compiler.warn_at(span, "value is unusually small")
		}
	}
}

const BUFFER_SIZE i64 = checked_buffer_size()

fn checked_buffer_size() i64 {
	const candidate i64 = 64
	warn_if_small(candidate, span: reflect.span_of(candidate))
	return require_non_zero(candidate, span: reflect.span_of(BUFFER_SIZE))
}

fn main() {
	assert BUFFER_SIZE == 64
}
