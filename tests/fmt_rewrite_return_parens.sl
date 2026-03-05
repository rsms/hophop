fn ret_scalar(x int) int {
return (x)
}

fn ret_expr(a int, b int) int {
return (a+b)
}

fn ret_tuple(a int, b int, c int) (int, int, int) {
return (a,b,c)
}

fn ret_nested_tuple(a int, b int) (int, int) {
return ((a,b))
}

fn ret_void(x int) {
if x > 0 {
return
}
}
