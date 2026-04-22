// Supports type function import named type no function import by providing the mem library package.
pub struct ArenaAllocator {
	block_size int
}

pub fn init(self *ArenaAllocator, block_size int) {
	self.block_size = block_size
}

pub fn free_all(self *ArenaAllocator) {
	self.block_size = 0
}
