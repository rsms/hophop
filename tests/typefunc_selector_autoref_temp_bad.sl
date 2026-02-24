struct S {
    x int
}

fn set_ptr(s *S, v int) {
    s.x = v
}

fn make_s() S {
    var s S
    return s
}

fn main() {
    make_s().set_ptr(1)
}
