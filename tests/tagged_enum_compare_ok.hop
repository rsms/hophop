// Verifies tagged enum compare is accepted.
enum Token u8 {
	End
	Int{
		value i32
	}
}

fn main() {
	var a Token = Token.Int{ value: 1 }
	var b Token = Token.Int{ value: 2 }
	var c Token = Token.End

	assert a != b
	assert !(a < b)
	assert c < a
}
