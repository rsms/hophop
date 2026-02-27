struct A {}

struct B {}

fn f_a(v A) {}

fn f_b(v B) {}

fn f { f_a, f_b }

fn main() {
	var n int
	f(n)
}
