fn example(message &str) context Context {
    var prefix = "message: "
    print(concat(prefix, message) with { mem = context.temp_mem })
}

fn main() {
    example("hello")
}
