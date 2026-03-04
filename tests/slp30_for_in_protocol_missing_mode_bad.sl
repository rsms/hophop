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

fn advance(it *BoxIterator, out *&Node) bool {
	if it.next != null {
		*out = it.next!
		it.next = null
		return true
	}
	return false
}

fn use_value(box &Box) i32 {
	for node in box {
		return node.value
	}
	return 0
}
