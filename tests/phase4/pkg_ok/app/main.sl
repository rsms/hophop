package app

import heap "ds/heap"

fn main() i32 {
    var b heap.Box = heap.Make(7)
    return b.v
}
