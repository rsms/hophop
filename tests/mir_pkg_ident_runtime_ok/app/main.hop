// Supports MIR package identifier runtime by providing the app entrypoint.
import "lib/dupe" as dupe

const Answer = 11

fn helper() i32 {
	return 7
}

fn call_through_value() i32 {
	var f = helper
	return f()
}

fn answer_value() i32 {
	return Answer
}

fn main() {
	assert dupe.helper() == 99
	assert call_through_value() == 7
	assert answer_value() == 11
}
