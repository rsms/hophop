// SLP-30: user-defined iterator protocol for `for ... in`
// - protocol hooks: `__iterator(source)` + `advance(it *Iter, out *T) bool`
// - supports by-value (`value`), ref (`&value`), and ptr (`*value`) bindings
// - can model finite and infinite iterables
// ---------- linked-list example ----------
struct ListEntry {
	value i32
	next  ?*ListEntry
}

struct MutList {
	head ?*ListEntry
}

struct List {
	head ?*ListEntry
}

struct MutListIterator {
	next ?*ListEntry
}

struct ListIterator {
	next ?*ListEntry
}

fn __iterator(list MutList) MutListIterator {
	return { next: list.head }
}

fn __iterator(list List) ListIterator {
	return { next: list.head }
}

fn advance(it *MutListIterator, out **ListEntry) bool {
	if it.next != null {
		var cur = it.next!
		*out = cur
		it.next = cur.next
		return true
	}
	return false
}

fn advance(it *ListIterator, out *&ListEntry) bool {
	if it.next != null {
		var cur = it.next!
		*out = cur
		it.next = cur.next
		return true
	}
	return false
}

fn advance(it *ListIterator, out *ListEntry) bool {
	if it.next != null {
		var cur = it.next!
		*out = *cur
		it.next = cur.next
		return true
	}
	return false
}

// ---------- infinite counter example ----------
struct Counter {
	start i32
}

struct CounterIterator {
	next i32
}

fn __iterator(counter Counter) CounterIterator {
	return { next: counter.start }
}

fn advance(it *CounterIterator, out *i32) bool {
	*out = it.next
	it.next += 1
	return true
}

fn main() {
	// SLP-29 sequence iteration still works.
	var a [i32 4]
	a[0] = 2
	a[1] = 4
	a[2] = 6
	a[3] = 8

	var seq_sum i32
	for i, item in a[:] {
		seq_sum += item * (i + 1) as i32
	}
	assert seq_sum == 60

	// Protocol iteration over a user-defined linked list.
	var e3 = ListEntry{ value: 3 }
	var e2 = ListEntry{ value: 2, next: &e3 }
	var e1 = ListEntry{ value: 1, next: &e2 }

	var mut_list = MutList{ head: &e1 }
	for *entry in mut_list {
		entry.value *= 10
	}

	var list = List{ head: mut_list.head }

	var sum_ref i32
	for &entry in list {
		sum_ref += entry.value
	}
	assert sum_ref == 60

	var sum_value i32
	for entry in list {
		sum_value += entry.value
	}
	assert sum_value == 60

	// Infinite iterator with explicit break.
	var first_five_sum i32
	for n in Counter{ start: 1 } {
		if n > 5 {
			break
		}
		first_five_sum += n
	}
	assert first_five_sum == 15
}
