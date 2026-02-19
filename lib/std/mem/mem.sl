pub struct Allocator {
    // Runtime ABI slot consumed by __sl_new/__sl_new_array.
    impl fn(self mut&Allocator, addr, align, curSize uint, newSizeInOut mut&uint, flags u32) uint
}

// Initialized by platform runtime before sl_main.
pub var platformAllocator mut&Allocator
