import "lib/fruit" { apple, banana, citrus, peach as not_a_plum }

fn main() i32 {
	return apple() + banana() + citrus() + not_a_plum()
}
