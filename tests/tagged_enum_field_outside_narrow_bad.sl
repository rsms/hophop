enum Token u8 {
	End
	Int{
		value i32
	}
}

fn read(t Token) i32 {
	return t.value
}
