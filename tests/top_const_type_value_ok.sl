// Verifies top-level const type value is accepted.
type UserId u32

const UserIdAlias = UserId

fn main() {
	assert type_name(UserIdAlias) == "UserId"
}
