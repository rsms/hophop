struct A {}

struct B {}

fn f(v A) {}

fn f(v B) {}

fn main() {
	var n int
	f(n)
}
