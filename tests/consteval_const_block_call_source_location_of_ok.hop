// Verifies const-eval const block call source_location_of is accepted.
import "builtin"
import "compiler"

fn require_non_zero(const value i64, const location builtin.SourceLocation) i64 {
	const {
		if value == 0 {
			compiler.error_at(location, "value must be non-zero")
		}
	}
	return value
}

fn warn_if_small(const value i64, const location builtin.SourceLocation) {
	const {
		if value < 16 {
			compiler.warn_at(location, "value is unusually small")
		}
	}
}

const BUFFER_SIZE i64 = checked_buffer_size()

fn checked_buffer_size() i64 {
	const candidate i64 = 64
	warn_if_small(candidate, location: builtin.source_location_of(candidate))
	return require_non_zero(candidate, location: builtin.source_location_of(BUFFER_SIZE))
}

fn main() {
	assert BUFFER_SIZE == 64
}
