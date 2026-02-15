fn cleanup() void {}

fn main() i32 {
    defer cleanup()
    return 0
}
