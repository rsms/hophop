import "lib/coll" as coll

fn is_one(t coll.Collection.Tag) bool {
	return t == coll.Collection.Tag.One
}

fn main() {
	var c  = coll.Collection{}
	var v  = coll.Collection.Value{ i: 7 }
	var it = coll.Collection.Item{ parent: &c, value: v }
	assert it.value.i == 7
	assert is_one(coll.Collection.Tag.One)
}
