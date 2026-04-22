import "builtin"

pub fn failing_alloc_impl(_self *builtin.MemAllocator, _addr rawptr, _align, _curSize int, _newSizeInOut *int, _flags u32, _srcLoc builtin.SourceLocation) rawptr {
	return null as rawptr
}

pub struct FailingAllocator {
	builtin.MemAllocator = { handler: failing_alloc_impl }
}
