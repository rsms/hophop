// reflection phase-2: `type`, `typeof`, `kind`, `base`, `is_alias`, `type_name`
type UserId u32

struct Pair {
	a, b i32
}

const K_USER_ID = UserId.kind()

const K_U32 = u32.kind()

const T_USER_BASE type = UserId.base()

const USER_IS_ALIAS = is_alias(UserId)

const U32_IS_ALIAS = u32.is_alias()

const NAME_USER = type_name(UserId)

const NAME_U32 = u32.type_name()

fn ptr_of(T type) type {
	return ptr(T)
}

const I64Ptr = ptr_of(i64)

const U8x4 = array(u8, N: 4 as uint)

fn main() {
	assert typeof(1 as i32) == i32
	assert typeof(u32) == type

	assert K_USER_ID != K_U32
	assert T_USER_BASE == u32
	assert USER_IS_ALIAS
	assert !U32_IS_ALIAS
	assert NAME_USER == "UserId"
	assert NAME_U32 == "u32"

	assert Pair.kind() != u32.kind()
	assert kind(UserId) == UserId.kind()
	assert base(UserId) == u32
	assert !is_alias(Pair)
	assert type_name(typeof(u32)) == "type"

	var x i64    = 12 as i64
	var p I64Ptr = &x
	assert *p == (12 as i64)

	var bytes U8x4
	bytes[0] = 3 as u8
	assert bytes[0] == (3 as u8)
}
