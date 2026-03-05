// Iterator protocol for `for ... in`
// - protocol hooks: `__iterator(source)` + `next_value(it *Iter) ?T`
// - supports by-value (`value`) and by-reference (`&value`) bindings
// - can model finite and infinite iterables
// ---------- linked-list example ----------
struct ListEntry {
	value i32
	next  ?*ListEntry
}

struct List {
	head ?*ListEntry
}

struct MutListIterator {
	next ?*ListEntry
}

struct ListIterator {
	next ?&ListEntry
}

fn __iterator(list *List) MutListIterator {
	return { next: list.head }
}

fn __iterator(list &List) ListIterator {
	return { next: list.head }
}

fn next_value(it *MutListIterator) ?*ListEntry {
	var cur = it.next
	if cur != null {
		it.next = cur.next
	}
	return cur
}

fn next_value(it *ListIterator) ?&ListEntry {
	var cur = it.next
	if cur != null {
		it.next = cur.next
	}
	return cur
}

fn modify(list *List) {
	for &entry in list {
		entry.value *= 10
	}
}

fn sum_ref(list &List) i32 {
	var acc i32
	for &entry in list {
		acc += entry.value
	}
	return acc
}

fn sum_value(list &List) i32 {
	var acc i32
	for entry in list {
		acc += entry.value
	}
	return acc
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

fn next_value(it *CounterIterator) ?i32 {
	var cur = it.next
	it.next += 1
	return cur
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

	var list = List{ head: &e1 }
	modify(&list)
	assert sum_ref(&list) == 60
	assert sum_value(&list) == 60

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
