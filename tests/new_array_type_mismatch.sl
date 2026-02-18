fn foo(ma mut&MemAllocator) {
    var many *i32 = new(&ma, i32, 8)
    _ = many
}
