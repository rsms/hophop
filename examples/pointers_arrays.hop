// pointers, fixed-size arrays, indexing, and dereference operations
struct Node {
	value i32
	next  *Node
}

fn main() {
	var values [i32 4]
	values[0] = 7

	var p          = &values[0]
	var n    Node
	var none *Node = (null as rawptr) as *Node
	n.value = *p
	n.next = none

	if n.next == none {
		assert n.value == 7
	} else {
		assert 0 == 1
	}
}
