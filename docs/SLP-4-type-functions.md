# SLP-4 type functions

> Status: Daft

Allows calling functions which takes a type `T` as its first argument via `expr.f`.

Really just syntax sugar that make the two statements of this function equivalent:

```sl
fn something(f &Foo, n i32)
fn example(f &Foo) {
    something(f, 123)
    f.something(123)
}
```

## Multiple dispatch

Do we need multiple dispatch and/or generics/macros for this to make sense? I.e. if I want to do `x.update` and `y.update` where `x` and `y` are different types that want different `update` function implementations?

Challenging example:

```sl
struct Pet {}
struct Spaceship {}
fn update(pet mut&Pet, timestamp u64)
fn update(ship mut&Spaceship)
fn example(pet mut&Pet, ship mut&Spaceship, timestamp u64) {
    pet.update(timestamp)
    ship.update()
}
```

Ideas for solutions:

1. Simply not possible; one package can only have one function named `update`, regardless of types.

2. Allow multiple definitions of a function, where the identity of a function becomes name + parameter types. C++ and a few other languages works this way. The downside is that errors may become confusing, where `ship.updatez(123)` would say something like "no matching 'updatez' function for (mut&Spaceship, int)" rather than the much clearer "no function named 'updatez'" or "unexpected extra argument for function 'update', expected (mut&Spaceship), got (mut&Spaceship, int)"`

3. Explicit multiple dispatch, similar to Odin's explicit function overloading (see https://odin-lang.org/docs/overview/#explicit-procedure-overloading). E.g.
    ```sl
    fn update_pet(pet mut&Pet, timestamp u64)
    fn update_ship(ship mut&Spaceship)
    fn update{update_pet, update_ship}
    ```

4. Allow functions to be tied to specific types via declaration. I.e. `fn Pet.update(pet mut&, timestamp u64)`, `fn Spaceship.update(ship mut&)` and `fn update()` are all distinct functions. Downside is more complex syntax. As a bonus, this approach would allow declaring functions as part of struct definitions, i.e. `struct Monster { x int; update(monster &mut) { ... } }`

5. `_Generic`-like approach with macros. I.e. `macro update(self, args...) $switch(typeof(self)) { case mut&Pet: update_pet(self, args...); case mut&Spaceship: update_ship(self, args...); }`. This implies introducing a macro system, which is a rather big project.

I like option 4 and 3. What do you think?
