fn noop() {
}

fn example(a, _, c int) int {
    var _ = noop()
    const _ = noop()
    return a + c
}
