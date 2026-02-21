struct Base {
    x i32
}

struct Bad {
    Base = Base{ x = 1 }
    y i32
}
