fn cleanup() {}

fn main() i32 {
    defer cleanup()
    return 0
}
