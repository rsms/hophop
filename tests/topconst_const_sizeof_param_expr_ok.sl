fn elem_size(x i32) i32 {
    return sizeof(x) as i32
}

const ELEM_SIZE = elem_size(42)
var BUF [u8 elem_size(0)]

fn main() {
    assert ELEM_SIZE == 4
    assert len(BUF) == 4
}
