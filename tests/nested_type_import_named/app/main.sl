import "lib/coll" { Collection }

fn is_one(t Collection.Tag) bool {
	return t == Collection.Tag.One
}

fn main() {
	var c = Collection{}
	var v  Collection.Value
	var it Collection.Item
	v.i = 9
	it.parent = &c
	it.value = v
	assert it.value.i == 9
	assert is_one(Collection.Tag.One)
}
