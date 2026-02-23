pub struct str {
    len u32
    bytes [u8 .len]
}

pub struct Allocator {
    impl fn(self *Allocator, addr, align, curSize uint, newSizeInOut *uint, flags u32) uint
}
pub struct Context {
    mem *Allocator
    console i32
    stderr i32
}
