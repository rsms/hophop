// Verifies formatter output for generic literal-cast rewrites.
struct Vector[T] {
x, y T
}

fn add[T](a, b Vector[T]) Vector[T] {
return { x: a.x + b.x, y: a.y + b.y }
}

fn scale[T](self Vector[T], k T) Vector[T] {
return { x: self.x * k, y: self.y * k }
}

fn first[T](x, y T) T {
return x
}

fn main() {
var v1 Vector[i64]=Vector[i64]{ x:-1 as i64, y:4 }
var v2 Vector[i64]=Vector[i64]{ x:5, y:-2 }
var sum=add(v1,b:v2)
var doubled=v1.scale(k:2 as i64)
var chain=v1.scale(k:2 as i64).add(v2).add(sum)
var keep=first(1 as i8,_:2)
assert sum.x==(4 as i64)
assert doubled.x==-2 as i64
assert chain.y==(8 as i64)
assert typeof(keep)==i8
}
