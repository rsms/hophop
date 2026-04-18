// Verifies tagged enum switch narrow is accepted.
enum Token u8 {
	End
	Int{
		value i32
	}
	Pair{
		left  i32
		right i32
	}
}

fn measure(t Token) i32 {
	switch t {
		case Token.End       { return 0 }
		case Token.Int       { return t.value }
		case Token.Pair as p { return p.left + p.right }
	}
}

fn main() {
	var a Token = Token.Int{ value: 7 }
	var b Token = Token.Pair{ left: 2 }
	assert measure(a) == 7
	assert measure(b) == 2
}
