// Verifies unknown ambient context fields are rejected.
import "platform"

fn write_err() {
	var e i32 = context.stdin
}
