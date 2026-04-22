// Verifies type const alias non distinct is accepted.
type UserId u32

const UserIdAlias = UserId

fn id(v UserIdAlias) UserIdAlias {
	return v
}

fn main() {
	var a UserId      = 7 as UserId
	var b UserIdAlias = id(a)
	assert type_name(typeof(b)) == "UserId"
	assert b == a
}
