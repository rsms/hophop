struct MutList {
	head ?*ListEntry
}

struct List {
	head ?*ListEntry
}

struct ListEntry {
	value i32
	next  ?*ListEntry
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

fn advance(it *MutListIterator, result **ListEntry) bool {
	if it.next != null {
		var cur = it.next!
		*result = cur
		it.next = cur.next
		return true
	}
	return false
}

fn advance(it *ListIterator, result *&ListEntry) bool {
	if it.next != null {
		var cur = it.next!
		*result = cur
		it.next = cur.next
		return true
	}
	return false
}

fn advance(it *ListIterator, result *ListEntry) bool {
	if it.next != null {
		var cur = it.next!
		*result = *cur
		it.next = cur.next
		return true
	}
	return false
}

fn modify(list MutList) {
	for *entry in list {
		entry.value *= 10
	}
}

fn sum_ref(list List) i32 {
	var acc i32
	for &entry in list {
		acc += entry.value
	}
	return acc
}

fn sum_value(list List) i32 {
	var acc i32
	for entry in list {
		acc += entry.value
	}
	return acc
}

fn main() {
	var entry3 = ListEntry{ value: 3 }
	var entry2 = ListEntry{ value: 2, next: &entry3 }
	var entry1 = ListEntry{ value: 1, next: &entry2 }
	var list   = MutList{ head: &entry1 }
	var ro     = List{ head: list.head }

	modify(list)
	assert sum_ref(ro) == 60
	assert sum_value(ro) == 60
}
