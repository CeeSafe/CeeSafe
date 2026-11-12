---
title: Programming a guessing game
summary: A first project built in five steps - a number-guessing game that reads a line of input, makes a random secret number, turns text into a number, compares, loops, and handles a guess that is not a number.
assumes: [ch01-02-hello-world]
keywords: [Console.read, Random.fill, parseInt, trim, Vector, StringView.fromUtf8, loop, while, match, break, continue, optional]
diagnostics: [N0207, N0249]
---

# Programming a guessing game

**This chapter teaches** by building one small program: a game in which the program chooses a secret number from 1
to 100 and you guess it. It assumes you can write, run and build a program in one file
([Hello, world](./ch01-02-hello-world)).

The game uses many things that later chapters teach properly: variables, functions, loops, borrows, optionals and
errors. Here each one gets a sentence that says what it does and names the chapter that covers it. You do not need
to understand every line yet. Type the listings, run them, and change them.

The game grows in five steps. Every listing is the whole of `main.cee`.

## Build this program, do not interpret it

Chapter 1 ran programs with `ceesafec run --interp`. For this chapter, build the program and run the executable:

```sh
ceesafec build main.cee
./main
```

There are two reasons, and both come from the interpreter being made for runs that can be repeated exactly:

- The interpreter shows what a program prints when the program ends. A game has to print a hint before you type the
  next guess.
- In the interpreter, the source of random bytes gives the same bytes on every run. The secret number would be the
  same every time.

`ceesafec check main.cee` is still the quickest way to find errors while you type.

## Step 1: reading a guess

CeeSafe has one function that reads standard input: `Console.read`. It fills a buffer with bytes and returns how
many it read. It has no function that reads a line, so the first step writes one, called `readLine`.

<span class="listing-caption"><strong>Listing 2-1</strong>, <code>main.cee</code>: Read one line and print it back.</span>

```ceesafe
fn readLine(line: borrow mut Vector<u8>) -> bool {
    line.clear();
    var byte = [0u8; 1];
    loop {
        let count = Console.read(mut byte[..])!;
        if (count == 0) {
            return line.length > 0;
        }
        if (byte[0] == '\n') {
            return true;
        }
        line.push(byte[0])!;
    }
}

fn main() {
    var line = Vector<u8>.create();
    Console.writeLine("Please type your guess.");
    if (readLine(mut line)) {
        let guess = StringView.fromUtf8(line.view()) ?? "";
        Console.write("You guessed: ");
        Console.writeLine(guess.trim());
    }
}
```

Build it, run it, type `50` and press Enter. The session looks like this:

```text
Please type your guess.
50
You guessed: 50
```

Start with `main`:

- `var line = Vector<u8>.create();` makes an empty vector of bytes and names it `line`. A `u8` is one byte, and a
  `Vector` is a list that can grow. `var` declares a variable that changes; `let` declares one that does not.
  Chapter 3 covers variables and types, and chapter 8 covers `Vector`.
- `readLine(mut line)` calls the function above `main`. The word `mut` shows that the call may change `line`.
- `if (…) { … }` runs its block only when the condition is true. `readLine` returns `false` when there is no more
  input, and then the program ends without printing a guess. Chapter 3 covers `if`.
- `StringView.fromUtf8(line.view())` looks at the bytes as text. A `StringView` is a view of text that is stored
  somewhere else, here in `line`. Bytes are not always valid text, so the result is an **optional**: it holds a
  value or it is `null`. `?? ""` says what to use when it is `null`: an empty string. Chapter 6 covers optionals.
- `guess.trim()` is the text without the spaces at its ends. On Windows a line ends with two characters, and `trim`
  also removes the one that `readLine` leaves.

Now `readLine`:

- `fn readLine(line: borrow mut Vector<u8>) -> bool` declares a function with one parameter, `line`, that returns
  a `bool`: `true` or `false`. Chapter 3 covers functions.
- `borrow mut` means the function changes the caller's vector where it is and does not take it away. That is a
  **borrow**, and chapter 4, on ownership, is about it.
- `var byte = [0u8; 1];` is an array of one byte, set to zero. It is the buffer `Console.read` fills.
- `loop { … }` repeats its block until a `return` or a `break` leaves it.
- `Console.read(mut byte[..])` reads at most one byte, because the buffer holds one. `byte[..]` is the whole array
  as a slice, which chapter 4 covers. The call can fail, so it returns a `Result`: the count, or an error. The `!`
  after it takes the count, and stops the program if there was an error. Chapter 9 covers errors.
- A count of `0` means the input has ended. The function then returns `true` only if the last line had something
  in it.
- `'\n'` is the character that ends a line. When the byte is that character the line is complete.
- `line.push(byte[0])!;` adds the byte to the vector. `push` needs memory and can fail, so it also takes a `!`.

::: tip Rule
A call that returns a `Result` or an optional does not give you the value directly. `!` takes the value and stops
the program if there is none. `?? value` gives an optional a fallback.
:::

## Step 2: a secret number

`Random.fill` fills a buffer with random bytes from the operating system. It has no function for a random number
in a range, so the program makes one from four bytes.

<span class="listing-caption"><strong>Listing 2-2</strong>, <code>main.cee</code>: Make a number from 1 to 100 and print it.</span>

```ceesafe
fn main() {
    var bytes = [0u8; 4];
    Random.fill(mut bytes[..])!;
    let secret = bytes[..].readLE<u32>(0) % 100 + 1;
    Console.write("The secret number is ");
    Console.writeLine(secret);
}
```

```text
The secret number is 82
```

- `var bytes = [0u8; 4];` is an array of four bytes.
- `Random.fill(mut bytes[..])!;` fills it. Like `Console.read`, the call can fail and takes a `!`.
- `bytes[..].readLE<u32>(0)` reads the four bytes, starting at index 0, as one `u32`: a whole number from 0 to
  4,294,967,295.
- `% 100` is the remainder after dividing by 100, a number from 0 to 99. Adding 1 gives 1 to 100.
- `let secret` never changes, so it is a `let`. The compiler works out that its type is `u32`.

The output above is what `ceesafec run --interp main.cee` prints, and it prints 82 on every run. The built program
prints a different number each time. Two runs of `./main` gave:

```text
The secret number is 81
The secret number is 47
```

## Step 3: comparing the guess with the secret

A guess arrives as text, and the secret is a number. `parseInt` turns text into a number.

<span class="listing-caption"><strong>Listing 2-3</strong>, <code>main.cee</code>: One guess, compared with the secret.</span>

```ceesafe
fn readLine(line: borrow mut Vector<u8>) -> bool {
    line.clear();
    var byte = [0u8; 1];
    loop {
        let count = Console.read(mut byte[..])!;
        if (count == 0) {
            return line.length > 0;
        }
        if (byte[0] == '\n') {
            return true;
        }
        line.push(byte[0])!;
    }
}

fn main() {
    var bytes = [0u8; 4];
    Random.fill(mut bytes[..])!;
    let secret = bytes[..].readLE<u32>(0) % 100 + 1;

    Console.writeLine("Guess the number from 1 to 100.");
    var line = Vector<u8>.create();
    if (readLine(mut line)) {
        let text = StringView.fromUtf8(line.view()) ?? "";
        let guess = text.trim().parseInt<u32>()!;
        if (guess < secret) {
            Console.writeLine("Too small!");
        } else if (guess > secret) {
            Console.writeLine("Too big!");
        } else {
            Console.writeLine("You win!");
        }
    }
}
```

One run, with the guess `50`:

```text
Guess the number from 1 to 100.
50
Too small!
```

Your run may say `Too big!`, because your secret is another number.

- `text.trim().parseInt<u32>()` reads the text as a `u32`. The type in `<` `>` says which kind of number you want.
  Text such as `fifty` is not a number, so the result is an optional. For now the `!` takes the number and stops
  the program when there is none. Step 5 does better.
- `if … else if … else` tries each condition in turn and runs one block. `guess` and `secret` are both `u32`, so
  `<` and `>` can compare them.

::: tip Rule
The two sides of a comparison have the same type. Text is turned into a number first, with `parseInt`.
:::

## Step 4: guessing until you win

One guess is not a game. To allow many, change the `if` that reads the line to a `while`, and leave the loop when
the guess is right.

<span class="listing-caption"><strong>Listing 2-4</strong>, <code>main.cee</code>: The game asks until the guess is right.</span>

```ceesafe
fn readLine(line: borrow mut Vector<u8>) -> bool {
    line.clear();
    var byte = [0u8; 1];
    loop {
        let count = Console.read(mut byte[..])!;
        if (count == 0) {
            return line.length > 0;
        }
        if (byte[0] == '\n') {
            return true;
        }
        line.push(byte[0])!;
    }
}

fn main() {
    var bytes = [0u8; 4];
    Random.fill(mut bytes[..])!;
    let secret = bytes[..].readLE<u32>(0) % 100 + 1;

    Console.writeLine("Guess the number from 1 to 100.");
    var line = Vector<u8>.create();
    while (readLine(mut line)) {
        let text = StringView.fromUtf8(line.view()) ?? "";
        let guess = text.trim().parseInt<u32>()!;
        if (guess < secret) {
            Console.writeLine("Too small!");
        } else if (guess > secret) {
            Console.writeLine("Too big!");
        } else {
            Console.writeLine("You win!");
            break;
        }
    }
}
```

One run:

```text
Guess the number from 1 to 100.
50
Too small!
75
Too big!
62
Too big!
56
Too big!
53
Too big!
51
You win!
```

- `while (readLine(mut line)) { … }` calls `readLine` before each turn and runs the block while it returns `true`.
  So the game also ends when the input ends: on macOS and Linux, press Ctrl-D to give up.
- `readLine` begins with `line.clear()`, so each turn starts with an empty vector. One vector serves the whole
  game.
- `break;` leaves the loop. After the loop `main` ends, and so does the program.

Chapter 3 covers `loop`, `while`, `break` and `continue`.

The game has one fault left. Another run, with a guess that is not a number:

```text
Guess the number from 1 to 100.
50
Too big!
fifty
ceesafe panic: unwrapped null at main.cee:25:21
```

`parseInt` returned `null`, and the `!` on line 25 stopped the program. That stop is called a **panic**.

## Step 5: a guess that is not a number

A player who types a word should be asked again. Replace the `!` with a `match`, which looks at the optional and
chooses what to do for each case.

<span class="listing-caption"><strong>Listing 2-5</strong>, <code>main.cee</code>: The finished game.</span>

```ceesafe
fn readLine(line: borrow mut Vector<u8>) -> bool {
    line.clear();
    var byte = [0u8; 1];
    loop {
        let count = Console.read(mut byte[..])!;
        if (count == 0) {
            return line.length > 0;
        }
        if (byte[0] == '\n') {
            return true;
        }
        line.push(byte[0])!;
    }
}

fn main() {
    var bytes = [0u8; 4];
    Random.fill(mut bytes[..])!;
    let secret = bytes[..].readLE<u32>(0) % 100 + 1;

    Console.writeLine("Guess the number from 1 to 100.");
    var line = Vector<u8>.create();
    while (readLine(mut line)) {
        let text = StringView.fromUtf8(line.view()) ?? "";
        let guess = match (text.trim().parseInt<u32>()) {
            Some(number) => number,
            null => {
                Console.writeLine("Please type a number.");
                continue;
            }
        };
        if (guess < secret) {
            Console.writeLine("Too small!");
        } else if (guess > secret) {
            Console.writeLine("Too big!");
        } else {
            Console.writeLine("You win!");
            break;
        }
    }
}
```

One run:

```text
Guess the number from 1 to 100.
50
Too small!
fifty
Please type a number.
75
Too big!
62
Too big!
56
Too big!
53
Too big!
51
You win!
```

- `match (…) { … }` has one **arm** for each case of the optional. Chapter 6 covers `match`.
- `Some(number) => number` is the arm for an optional that holds a value. It names the value `number`, and the
  whole `match` has that value, which becomes `guess`.
- `null => { … }` is the arm for no value. It prints a message, and `continue;` goes straight to the next turn of
  the loop, so the lines below it do not run and `guess` is never used without a number.

The three `!` that remain are on calls that fail only when the system cannot read input, give random bytes or
give memory. Stopping is a reasonable answer to those in a game. Chapter 9 shows how to report such an error
instead.

## Mistakes

::: warning Mistake: comparing text with a number
A line of input is text, even when it looks like a number. The compiler does not convert it for you.
:::

<span class="listing-caption"><strong>Listing 2-6</strong>, <code>main.cee</code>: <code>guess</code> is text and <code>secret</code> is a number. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
fn main() {
    let secret = 82;
    let guess = "50";
    if (guess < secret) {
        Console.writeLine("Too small!");
    }
}
```

```text
main.cee:4:9: error[N0207]: operator '<' requires operands of the same type, found 'StringView' and 'i32'
  |
4 |     if (guess < secret) {
  |         ^^^^^^^^^^^^^^
```

Turn the text into a number first, as Listing 2-3 does: `guess.parseInt<i32>()`, then `!` or a `match`.

::: warning Mistake: leaving out `mut` at the call
A function that changes its argument has a `borrow mut` parameter, and the call must say `mut` too.
:::

<span class="listing-caption"><strong>Listing 2-7</strong>, <code>main.cee</code>: The call does not say <code>mut</code>. This does not compile.</span>

<!-- ceesafe: fail N0249 -->
```ceesafe
fn clear(line: borrow mut Vector<u8>) {
    line.clear();
}

fn main() {
    var line = Vector<u8>.create();
    clear(line);
    Console.writeLine(line.length);
}
```

```text
main.cee:7:11: error[N0249]: this parameter is 'borrow mut': write 'mut' before the argument to let the call modify it
  |
7 |     clear(line);
  |           ^^^^
```

Write `clear(mut line)`. [Borrowing](./ch04-02-borrowing) explains the rule.

::: warning Mistake: `!` on a value that comes from a person
`!` is for a value you are sure is there. Text that a person types may be anything, and a wrong guess of yours
becomes a panic.
:::

<span class="listing-caption"><strong>Listing 2-8</strong>, <code>main.cee</code>: The text is not a number. This compiles, and stops when it runs.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    let guess = "fifty".parseInt<u32>()!;
    Console.writeLine(guess);
}
```

The program prints nothing to standard output. It writes this to standard error and ends:

```text
ceesafe panic: unwrapped null at main.cee:2:17
```

Handle the `null` case with a `match`, as Listing 2-5 does, or give a fallback with `??`.

::: details Coming from Rust, Python, C# or Go
There is no `read_line`, `input()` or `Console.ReadLine()`: you read bytes with `Console.read` and find the end of
the line yourself. There is no `rand` package to add and no `gen_range`: `Random.fill` gives bytes, and you make
the number. An optional is `Some(x)` or `null`, not `None`, and `!` after an expression is the unwrap, not a
"not".
:::

## Summary

- Build an interactive program with `ceesafec build` and run the executable. The interpreter prints at the end of
  the run and its random bytes are the same every time.
- `Console.read(mut buffer)` reads bytes from standard input and returns how many; `0` means the input has ended.
- `Random.fill(mut buffer)` fills a buffer with random bytes.
- `text.parseInt<u32>()` turns text into a number. The result is an optional: `null` for text that is not a number.
- `!` takes the value from a `Result` or an optional and panics if there is none. `??` gives an optional a
  fallback. `match` handles each case.
- The two sides of a comparison have the same type (`N0207`).
- An argument that the call may change is written `mut x` (`N0249`).
- `loop` and `while` repeat a block; `break` leaves the loop and `continue` starts its next turn.

Next: [chapter 3](./ch03-00-common-concepts) teaches variables, types, functions and control flow one at a time.
