// type-function selector sugar with user-defined and built-in functions
struct Counter {
	value int
}

fn doubled(c Counter) int {
	return c.value * 2
}

fn add(c Counter, d int) int {
	return c.value + d
}

fn main() {
	var c Counter
	c.value = 21

	var msg = "hi"
	var ma  = 0 as *Allocator

	assert c.doubled() == doubled(c)
	assert c.add(7) == add(c, d: 7)
	assert msg.len() == len(msg)

	var p ?*i32     = new i32 with ma
	var q ?*[i32 4] = new [i32 4] with ma

	assert p == null
	assert q == null
}
