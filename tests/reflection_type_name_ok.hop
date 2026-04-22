// Verifies reflection type name is accepted.
type UserId u32

const NAME_USER = type_name(UserId)

const NAME_U32 = u32.type_name()

const NAME_META = type_name(typeof(u32))

fn main() {
	assert NAME_USER == "UserId"
	assert NAME_U32 == "u32"
	assert NAME_META == "type"
	assert type_name(UserId) == UserId.type_name()
	assert type_name(typeof(1 as i32)) == "i32"
}
