// Verifies topconst const cycle unused is rejected.
const A = B

const B = A

fn main() {}
