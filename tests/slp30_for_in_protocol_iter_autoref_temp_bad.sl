// Verifies SLP 30 for in protocol iterator autoref temporary is rejected.
struct Counter {
	start i32
	done  bool
}

struct CounterIterator {
	next i32
	done bool
}

fn __iterator(counter &Counter) CounterIterator {
	return { next: counter.start, done: counter.done }
}

fn next_value(it *CounterIterator) ?*i32 {
	if it.done {
		return null
	}
	it.done = true
	return &it.next
}

fn first() i32 {
	for n in Counter{ start: 5 } {
		return n
	}
	return 0
}
