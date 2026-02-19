// function groups (explicit overload sets) and selector-call dispatch

struct Cat {
    score int
}

struct Dog {
    score int
}

fn pick_cat(v Cat) int {
    return v.score
}

fn pick_dog(v Dog) int {
    return v.score
}

fn pick{pick_cat, pick_dog}

fn main() {
    var cat Cat
    var dog Dog

    cat.score = 9
    dog.score = 4

    assert pick(cat) == 9
    assert dog.pick() == 4
}
