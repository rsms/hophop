type MyInt int

fn takesAlias(x MyInt) {
}

fn main() {
    var x int = 1
    takesAlias(x)
}
