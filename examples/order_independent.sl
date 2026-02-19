// declaration-order independence for functions and types

fn main() {
    assert twice(9) == 18
}

fn twice(x i32) i32 {
    return x * 2
}
