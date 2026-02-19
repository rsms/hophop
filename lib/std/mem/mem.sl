pub struct Allocator {
    // Runtime ABI slot consumed by __sl_new/__sl_new_array.
    impl uint
}

pub struct PlatformAllocator {
    Allocator
}

// Initialized by platform runtime before sl_main.
pub var platformAllocator mut&PlatformAllocator
