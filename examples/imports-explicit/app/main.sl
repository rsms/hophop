// Example: importing a package with an explicit alias when path tail is not an identifier.

import math "lib/math-v2"

fn main() {
    assert math.Double(21) == 42
}
