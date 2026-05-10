# Rosetta Code Progress

This tracks HopHop implementations of Rosetta Code tasks.

## First batch

| Rosetta task | HopHop file | Status | Missing features | Notes |
| --- | --- | --- | --- | --- |
| Hello world/Text | `examples/rosetta/hello_world_text.hop` | implemented: eval, C, wasm | none | stdout smoke test |
| FizzBuzz | `examples/rosetta/fizz_buzz.hop` | implemented: eval, C, wasm | none | prints 1 through 100 |
| Factorial | `examples/rosetta/factorial.hop` | implemented: eval, C, wasm | none | recursive and iterative implementations |
| Fibonacci sequence | `examples/rosetta/fibonacci_sequence.hop` | implemented: eval, C, wasm | none | first 20 terms |
| Greatest common divisor | `examples/rosetta/greatest_common_divisor.hop` | implemented: eval, C, wasm | none | Euclid algorithm |
| Least common multiple | `examples/rosetta/least_common_multiple.hop` | implemented: eval, C, wasm | none | derived from GCD |
| 100 doors | `examples/rosetta/one_hundred_doors.hop` | implemented: eval, C, wasm | none | toggles 100 bool slots |
| Sum and product of an array | `examples/rosetta/sum_and_product_of_an_array.hop` | implemented: eval, C, wasm | none | fixed integer array |
| Dot product | `examples/rosetta/dot_product.hop` | implemented: eval, C, wasm | none | fixed integer vectors |
| Sieve of Eratosthenes | `examples/rosetta/sieve_of_eratosthenes.hop` | implemented: eval, C; wasm blocked | wasm `format` call support | variable limit, primes through 100 |
| Sorting algorithms/Bubble sort | `examples/rosetta/bubble_sort.hop` | implemented: eval, C, wasm | none | sorts a fixed integer array |
| Binary search | `examples/rosetta/binary_search.hop` | implemented: eval, C, wasm | none | fixed sorted array |

## Second batch

| Rosetta task | HopHop file | Status | Missing features | Notes |
| --- | --- | --- | --- | --- |
| ROT13 | `examples/rosetta/rot13.hop` | implemented: eval, C, wasm | none | byte-oriented ASCII transformation |
| Reverse a string | `examples/rosetta/reverse_a_string.hop` | implemented: eval, C, wasm | none | byte-oriented ASCII reversal |
| Palindrome detection | `examples/rosetta/palindrome_detection.hop` | implemented: eval, C, wasm | none | byte-oriented ASCII detection |
| Roman numerals/Encode | `examples/rosetta/roman_numerals_encode.hop` | implemented: eval, C, wasm | none | subtractive Roman numeral encoding |
| Look-and-say sequence | `examples/rosetta/look_and_say_sequence.hop` | implemented: eval, C, wasm | none | fixed buffers with explicit lengths |
| Pascal's triangle | `examples/rosetta/pascals_triangle.hop` | implemented: eval, C; wasm blocked | wasm `format` call support | first 7 rows |
| Perfect numbers | `examples/rosetta/perfect_numbers.hop` | implemented: eval, C; wasm blocked | wasm `format` call support | perfect numbers through 10000 |
| Amicable numbers | `examples/rosetta/amicable_numbers.hop` | implemented: eval, C; wasm blocked | wasm `format` call support | amicable pairs through 10000 |

## Deferred missing features

Some Rosetta Code tasks should wait for library or platform support:

| Feature area | Blocks |
| --- | --- |
| stdin and argv parsing | interactive tasks, command-line argument tasks |
| filesystem APIs | file I/O tasks |
| randomness | random number and simulation tasks |
| date and time APIs | calendar and clock tasks |
| networking | client/server tasks |
| richer string parsing and mutation helpers | text-heavy transformation tasks |
| maps and dictionaries | frequency tables, lookup-heavy tasks |
| graphics and GUI APIs | visual Rosetta tasks |
| concurrency APIs | threading, channels, and synchronization tasks |
| wasm `format` call support | numeric-output Rosetta tasks that use `"{}".format(value)` |
| wasm backend gaps beyond first-batch scalar/control-flow patterns | larger non-Rosetta programs under `wasm-min` |
