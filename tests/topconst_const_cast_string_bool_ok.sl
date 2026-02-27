const NONEMPTY bool = "x" as bool
const EMPTY bool = "" as bool

fn main() {
    assert NONEMPTY == true
    assert EMPTY == true
}
