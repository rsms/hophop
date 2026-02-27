import "slang/feature/optional"

pub fn maybe(x *i32) ?*i32 {
	return x
}

pub fn find(p ?*i32) bool {
	return p != null
}
