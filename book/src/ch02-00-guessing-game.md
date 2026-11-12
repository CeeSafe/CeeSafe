---
title: Programming a guessing game
summary: A first project built in five steps - a number-guessing game that reads a line of input, makes a random secret number, turns text into a number, compares, loops, and handles a guess that is not a number.
assumes: [ch01-02-hello-world]
keywords: [Console.readLine, Random.below, parseInt, trim, String, view, while, match, break, continue, optional, "run --interp"]
diagnostics: [N0207, N0249]
---

# Programming a guessing game

**This chapter teaches** by building one small program: a game in which the program chooses a secret number from 1
to 100 and you guess it. It assumes you can write, run and build a program in one file
([Hello, world](./ch01-02-hello-world)).

The game uses many things that later chapters teach properly: variables, loops, borrows, optionals and errors.
Here each one gets a sentence that says what it does and names the chapter that covers it. You do not need to
understand every line yet. Type the listings, run them, and change them.

The game grows in five steps. Every listing is the whole of `main.cee`.

## Two ways to run the game

Chapter 1 showed two ways to run a program, and the game works in both:

```sh
ceesafec run --interp main.cee
```

```sh
ceesafec build main.cee
./main
```

Either way the program prints its question, waits for you to type a line, and answers. There is one difference,
and it matters for a game:

- In the interpreter, random numbers follow a fixed sequence, so that a run can be repeated exactly. The secret
  number is the same on every run: it is 18.
- The built program asks the operating system for its random numbers. The secret is different on every run.

Use the interpreter while you write the game, because you know the answer and can test every hint. Build the
program to play it. `ceesafec check main.cee` is still the quickest way to find errors while you type.

## Step 1: reading a guess

`Console.readLine` reads one line from standard input, which is what you type in the terminal.

<span class="listing-caption"><strong>Listing 2-1</strong>, <code>main.cee</code>: Read one line and print it back.</span>

```ceesafe
fn main() {
    var line = String.create();
    Console.writeLine("Please type your guess.");
    if (Console.readLine(mut line)!) {
        Console.write("You guessed: ");
        Console.writeLine(line);
    }
}
```

Run it, type `50` and press Enter. The session looks like this:

```text
Please type your guess.
50
You guessed: 50
```

Line by line:

- `var line = String.create();` makes an empty string and names it `line`. A `String` is text that the program
  owns and that can grow. `var` declares a variable that changes; `let` declares one that does not. Chapter 3
  covers variables, and chapter 8 covers `String`.
- `Console.readLine(mut line)` waits for a line and puts its text in `line`, in place of what `line` held. The
  word `mut` shows that the call may change `line`. The function changes your string where it is and does not take
  it away: that is a **borrow**, and chapter 4, on ownership, is about it.
- The text in `line` does not include the end of the line, the character that the Enter key adds.
- Reading can fail, so `Console.readLine` does not return its answer directly. It returns a `Result`: the answer,
  or an error. The `!` after the call takes the answer, and stops the program if there was an error. Chapter 9
  covers errors.
- The answer is a `bool`: `true` when the function read a line, and `false` when there is no more input.
- `if (…) { … }` runs its block only when the condition is true. When the input has ended, the program ends
  without printing a guess. Chapter 3 covers `if`.
- `Console.write` prints without ending the line, so the guess follows on the same line.

::: tip Rule
A call that returns a `Result` does not give you the value directly. `!` takes the value, and stops the program if
there is an error instead.
:::

## Step 2: a secret number

`Random.below(n)` gives a random whole number from 0 up to, but not including, `n`.

<span class="listing-caption"><strong>Listing 2-2</strong>, <code>main.cee</code>: Make a number from 1 to 100 and print it.</span>

```ceesafe
fn main() {
    let secret = Random.below(100)! + 1;
    Console.write("The secret number is ");
    Console.writeLine(secret);
}
```

```text
The secret number is 18
```

- `Random.below(100)` is a number from 0 to 99. The random numbers come from the operating system, and asking for
  them can fail, so this call also returns a `Result` and takes a `!`.
- Adding 1 gives a number from 1 to 100.
- `let secret` never changes, so it is a `let`. The compiler works out its type from the value. `Random.below`
  gives a `u64`, a whole number that is never negative, so `secret` is a `u64`. Chapter 3 covers types.

The output above is what `ceesafec run --interp main.cee` prints, and it prints 18 on every run. The built program
prints a different number each time. Two runs of `./main` gave:

```text
The secret number is 48
The secret number is 71
```

## Step 3: comparing the guess with the secret

A guess arrives as text, and the secret is a number. `parseInt` turns text into a number.

<span class="listing-caption"><strong>Listing 2-3</strong>, <code>main.cee</code>: One guess, compared with the secret.</span>

```ceesafe
fn main() {
    let secret = Random.below(100)! + 1;

    Console.writeLine("Guess the number from 1 to 100.");
    var line = String.create();
    if (Console.readLine(mut line)!) {
        let guess = line.view().trim().parseInt<u64>()!;
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

One run in the interpreter, with the guess `50`:

```text
Guess the number from 1 to 100.
50
Too big!
```

The built program may say `Too small!`, because its secret is another number.

The line that makes `guess` does three things, from left to right:

- `line.view()` is a **view** of the text in `line`: a way to read the text without copying it. The functions that
  read text, such as `trim` and `parseInt`, work on a view. Chapter 4 covers views.
- `.trim()` is the text without the spaces at its ends, so a guess typed as ` 50 ` is still a number.
- `.parseInt<u64>()` reads the text as a `u64`. The type in `<` `>` says which kind of number you want. Text such
  as `fifty` is not a number, so the result is an **optional**: it holds a value or it is `null`. For now the `!`
  takes the number and stops the program when there is none. Step 5 does better. Chapter 6 covers optionals.

`if … else if … else` tries each condition in turn and runs one block. `guess` and `secret` are both `u64`, so `<`
and `>` can compare them.

::: tip Rule
The two sides of a comparison have the same type. Text is turned into a number first, with `parseInt`.
:::

## Step 4: guessing until you win

One guess is not a game. To allow many, change the `if` that reads the line to a `while`, and leave the loop when
the guess is right.

<span class="listing-caption"><strong>Listing 2-4</strong>, <code>main.cee</code>: The game asks until the guess is right.</span>

```ceesafe
fn main() {
    let secret = Random.below(100)! + 1;

    Console.writeLine("Guess the number from 1 to 100.");
    var line = String.create();
    while (Console.readLine(mut line)!) {
        let guess = line.view().trim().parseInt<u64>()!;
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

One run in the interpreter:

```text
Guess the number from 1 to 100.
50
Too big!
25
Too big!
12
Too small!
18
You win!
```

Each hint appears as soon as you press Enter, before the program waits for the next guess. In the interpreter the
same guesses always give this session. Build the program, and you have to find a new secret each time.

- `while (…) { … }` tests its condition before each turn and runs the block while the condition is true. Here the
  condition reads a line, so the game also ends when the input ends: on macOS and Linux, press Ctrl-D to give up.
- Each call of `Console.readLine` replaces the text in `line`. One string serves the whole game.
- `break;` leaves the loop. After the loop `main` ends, and so does the program.

Chapter 3 covers `while`, `break` and `continue`.

The game has one fault left. Another run, with a guess that is not a number:

```text
Guess the number from 1 to 100.
50
Too big!
fifty
ceesafe panic: unwrapped null at main.cee:7:21
```

`parseInt` returned `null`, and the `!` on line 7 stopped the program. That stop is called a **panic**.

## Step 5: a guess that is not a number

A player who types a word should be asked again. Replace the `!` with a `match`, which looks at the optional and
chooses what to do for each case.

<span class="listing-caption"><strong>Listing 2-5</strong>, <code>main.cee</code>: The finished game.</span>

```ceesafe
fn main() {
    let secret = Random.below(100)! + 1;

    Console.writeLine("Guess the number from 1 to 100.");
    var line = String.create();
    while (Console.readLine(mut line)!) {
        let guess = match (line.view().trim().parseInt<u64>()) {
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

One run in the interpreter:

```text
Guess the number from 1 to 100.
50
Too big!
fifty
Please type a number.
25
Too big!
12
Too small!
18
You win!
```

- `match (…) { … }` has one **arm** for each case of the optional. Chapter 6 covers `match`.
- `Some(number) => number` is the arm for an optional that holds a value. It names the value `number`, and the
  whole `match` has that value, which becomes `guess`.
- `null => { … }` is the arm for no value. It prints a message, and `continue;` goes straight to the next turn of
  the loop, so the lines below it do not run and `guess` is never used without a number.

The two `!` that remain are on calls that fail only when the system cannot give a random number or read the input.
Stopping is a reasonable answer to those in a game. Chapter 9 shows how to report such an error instead.

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
`Console.readLine` changes the string you give it, and the call must say so with `mut`.
:::

<span class="listing-caption"><strong>Listing 2-7</strong>, <code>main.cee</code>: The call does not say <code>mut</code>. This does not compile.</span>

<!-- ceesafe: fail N0249 -->
```ceesafe
fn main() {
    var line = String.create();
    if (Console.readLine(line)!) {
        Console.writeLine(line);
    }
}
```

```text
main.cee:3:26: error[N0249]: this parameter is 'borrow mut': write 'mut' before the argument to let the call modify it
  |
3 |     if (Console.readLine(line)!) {
  |                          ^^^^
```

Write `Console.readLine(mut line)`. [Borrowing](./ch04-02-borrowing) explains the rule.

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

Handle the `null` case with a `match`, as Listing 2-5 does, or give a fallback with `??`: `parseInt<u32>() ?? 0`
is 0 for text that is not a number.

::: details Coming from Rust, Python, C# or Go
`Console.readLine` fills a string that you own and returns whether there was a line; it does not return a new
string for each line, as `input()` and `Console.ReadLine()` do. There is no `rand` package to add:
`Random.below(100)` is Rust's `gen_range(0..100)` and Python's `randrange(100)`. An optional is `Some(x)` or
`null`, not `None`, and `!` after an expression is the unwrap, not a "not".
:::

## Summary

- The game runs with `ceesafec run --interp main.cee` and as a built program. In the interpreter random numbers
  are the same on every run, so the secret is always 18.
- `Console.readLine(mut line)` reads a line of standard input into a `String`, without the end of the line. Its
  value is `false` when the input has ended.
- `Random.below(n)` gives a random number from 0 to `n - 1`.
- `line.view()` is a view of a string's text, and `trim()` removes the spaces at its ends.
- `text.parseInt<u64>()` turns text into a number. The result is an optional: `null` for text that is not a number.
- `!` takes the value from a `Result` or an optional and panics if there is none. `??` gives an optional a
  fallback. `match` handles each case.
- The two sides of a comparison have the same type (`N0207`).
- An argument that the call may change is written `mut x` (`N0249`).
- `while` repeats a block; `break` leaves the loop and `continue` starts its next turn.

Next: [chapter 3](./ch03-00-common-concepts) teaches variables, types, functions and control flow one at a time.
