// Verifies topvar initializer cycle unused is rejected.
var A = B

var B = A

fn main() {}
