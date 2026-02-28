type UserId u32

struct Pair {
	a, b i32
}

const ALIAS_USER = is_alias(UserId)

const ALIAS_U32 = u32.is_alias()

fn main() {
	assert ALIAS_USER
	assert !ALIAS_U32
	assert UserId.is_alias()
	assert !Pair.is_alias()
	assert is_alias(UserId)
	assert !is_alias(i32)
}
