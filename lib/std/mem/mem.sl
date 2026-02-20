pub struct Allocator {
    // Runtime ABI slot consumed by __sl_new/__sl_new_array.
    // Contract:
    // - New allocation (addr == 0): returned bytes [0, newSize) are zero-initialized.
    // - Resize allocation (addr != 0): any grown region [curSize, newSize) is zero-initialized.
    impl fn(self mut&Allocator, addr, align, curSize uint, newSizeInOut mut&uint, flags u32) uint
}

// Initialized by platform runtime before sl_main.
pub var platformAllocator mut&Allocator
