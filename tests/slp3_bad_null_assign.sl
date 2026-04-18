// Verifies SLP 3 bad null assign.
import "slang/feature/optional"

pub fn bad() *i32 {
	return null
}
