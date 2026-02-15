pub {
    struct Box {
        v i32
    }

    fn Make(v i32) Box
}

fn Make(v i32) Box {
    var b Box
    b.v = v
    return b
}
