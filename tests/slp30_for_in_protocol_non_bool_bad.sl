struct Box {
	next ?&Node
}

struct Node {
	value i32
}

struct BoxIterator {
	next ?&Node
}

fn __iterator(box &Box) BoxIterator {
	return { next: box.next }
}

fn next_value(it *BoxIterator) uint {
	if it.next != null {
		it.next = null
		return 1
	}
	return 0
}

fn use_ref(box &Box) i32 {
	for &node in box {
		return node.value
	}
	return 0
}
