// importing a package with an explicit alias when path tail is not an identifier

import "lib/math-v2" as math

fn main() {
    assert math.Double(21) == 42
}
