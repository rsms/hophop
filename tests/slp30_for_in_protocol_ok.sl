// Verifies SLP 30 for in protocol is accepted.
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

struct Entry {
	key   &str
	value i32
}

type PairKV (&str, *i32)

struct Table {
	entries *[Entry]
}

struct TableIterator {
	entries *[Entry]
	index   uint
	pair    PairKV
}

var __dummy_i32 i32

fn __iterator(table *Table) TableIterator {
	return { entries: table.entries, pair: ("", &__dummy_i32) as PairKV }
}

fn next_key_and_value(it *TableIterator) ?*PairKV {
	if it.index >= len(it.entries) {
		return null
	}
	var entry = &it.entries[it.index]
	it.index += 1
	it.pair = (entry.key, &entry.value) as PairKV
	return &it.pair
}

fn main() {
	var entry3 = ListEntry{ value: 3 }
	var entry2 = ListEntry{ value: 2, next: &entry3 }
	var entry1 = ListEntry{ value: 1, next: &entry2 }
	var list   = List{ head: &entry1 }

	modify(&list)
	assert sum_ref(&list) == 60
	assert sum_value(&list) == 60

	var entries [Entry 2]
	entries[0] = Entry{ key: "aa", value: 2 }
	entries[1] = Entry{ key: "bbb", value: 3 }
	var table = Table{ entries: entries[:] }

	var key_len_sum uint
	var value_sum   i32
	for key, value in &table {
		key_len_sum += len(key)
		value_sum += value
	}
	assert key_len_sum == 5
	assert value_sum == 5

	for key, &value in &table {
		if len(key) == 2 {
			*value += 10
		}
	}
	assert entries[0].value == 12
	assert entries[1].value == 3

	var fallback_value_sum i32
	for value in &table {
		fallback_value_sum += value
	}
	assert fallback_value_sum == 15

	var key_count uint
	for key, _ in &table {
		key_count += len(key)
	}
	assert key_count == 5
}
