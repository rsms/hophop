struct Allocator {
    impl fn(self *Allocator, addr, align, curSize uint, newSizeInOut *uint, flags u32) uint
}

fn takes(cb fn(i32, &str) bool, plain fn()) fn(i32) i32 {
    var local fn(a, b i32) i32
    return local
}
