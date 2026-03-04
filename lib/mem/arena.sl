import "builtin"

pub struct ArenaBlock {
	next  *ArenaBlock
	addr  uint
	size  uint
	used  uint
	align uint
}

pub struct ArenaAllocator {
	builtin.Allocator
	mem        *builtin.Allocator
	head       *ArenaBlock
	block_size uint
}

fn alloc_block(arena *ArenaAllocator, minSize uint, align uint) *ArenaBlock {
	var payload_size = arena.block_size
	if payload_size < minSize {
		payload_size = minSize
	}

	if arena.mem == null as *builtin.Allocator {
		return null as *ArenaBlock
	}

	var payload_addr = arena.mem.impl(arena.mem, addr: 0, align, curSize: 0, newSizeInOut: &payload_size, flags: 0)
	if payload_addr == 0 {
		return null as *ArenaBlock
	}

	var block *ArenaBlock = new ArenaBlock with arena.mem
	block.next = arena.head
	block.addr = payload_addr
	block.size = payload_size
	block.used = minSize
	block.align = align
	arena.head = block
	return block
}

fn arena_alloc_impl(self *builtin.Allocator, addr uint, align uint, curSize uint, newSizeInOut *uint, flags u32) uint {
	var arena = self as *ArenaAllocator
	if newSizeInOut == null as *uint {
		return 0
	}

	if addr != 0 || curSize != 0 || flags != 0 {
		return 0
	}

	if align == 0 {
		return 0
	}

	var newSize uint = *newSizeInOut
	if newSize == 0 {
		return 0
	}

	var block = alloc_block(arena, minSize: newSize, align)
	if block == null as *ArenaBlock {
		return 0
	}
	return block.addr
}

pub fn init(self *ArenaAllocator, source *builtin.Allocator, block_size uint) {
	self.mem = source
	self.head = null as *ArenaBlock
	self.block_size = block_size
	if self.block_size == 0 {
		self.block_size = 4096
	}
	self.impl = arena_alloc_impl
}

fn free_block_chain(source *builtin.Allocator, block *ArenaBlock) {
	if block == null as *ArenaBlock {
		return
	}

	var next *ArenaBlock = block.next
	var zero uint        = 0
	source.impl(source, addr: block.addr, align: block.align, curSize: block.size, newSizeInOut: &zero, flags: 0)
	free(source, block)
	free_block_chain(source, block: next)
}

pub fn free_all(self *ArenaAllocator) {
	if self.mem == null as *builtin.Allocator {
		self.head = null as *ArenaBlock
		return
	}

	free_block_chain(self.mem, block: self.head)
	self.head = null as *ArenaBlock
}
