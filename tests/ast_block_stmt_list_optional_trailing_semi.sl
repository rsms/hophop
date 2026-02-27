fn parse_block_paths() {
    {}
    {
        ;
        ;
    }
    if true { 2 }
    if true { 3; }
    if true { ; 4 }
    if true { 5; ; }
    if true {
        6
        7;
    }
    if true {
        8;
        9
    }
    for {
        break
    }
    for {
        continue
    }
    if true {
        var x i32 = 1
    }
    if true {
        const y i32 = 2
    }
    if true {
        assert true
    }
    if true {
        return
    }
}
