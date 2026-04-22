import "builtin"

pub fn failing_alloc_impl(_self *builtin.Allocator, _addr rawptr, _align, _curSize int, _newSizeInOut *int, _flags u32) rawptr {
	return null as rawptr
}

pub struct FailingAllocator {
	builtin.Allocator = { impl: failing_alloc_impl }
}
