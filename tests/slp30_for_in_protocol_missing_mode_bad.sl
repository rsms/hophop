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

fn use_value(box &Box) i32 {
	for node in box {
		return node.value
	}
	return 0
}
