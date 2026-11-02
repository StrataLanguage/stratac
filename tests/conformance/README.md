# Conformance suite

Small programs that pin down what the language accepts, rejects and does at runtime, with the
ownership, borrowing and type rules in mind. Each `.strata` file is one ctest test
(`conformance/<accept|reject>/<name>`), compiled ahead of time with `stratac`, and for `accept`
cases linked with the C compiler and run.

`prelude.strata` is prepended to every case. It provides `printf`, a poisoning allocator (reads of
freed memory print `-572662307`, double frees print `!!! DOUBLE/BAD FREE`), a `Res` type with a
counting `Drop`, `Box(id)` and `Report()`, which prints `END live=N`.

```sh
ctest --preset default -L conformance            # the whole suite
ctest --preset default -R conformance/accept/     # one directory
ctest --preset default -L xfail                   # only the known holes
```

## Writing a case

The first lines are header comments:

```c
// expect: reject "used after move"
// rule: moves out of a variable make it unusable until it is assigned again
int main()
{
    ^Res a = Box(1);
    ^Res b = a;
    return a.id;
}
```

| Header | Meaning |
| --- | --- |
| `// expect: run` | Compiles, exits 0, no memory error, no panic, and `END live=0` if it calls `Report()` |
| `// expect: reject "text"` | Fails to compile (exit 1) with an error containing `text`; the text is optional |
| `// expect: panic "text"` | Compiles and panics with a message starting with `text`; the text is optional |
| `// expect-output: text` | That text appears in the program's output; repeat for several lines |
| `// rule: ...` | The rule the case pins, in one line |
| `// xfail: ...` | A known hole: the compiler does not do this yet, so the test is expected to fail |

`accept/` holds `run` and `panic` cases, `reject/` holds `reject` cases. Every case says what the
language *should* do. When it doesn't yet, add `// xfail:` with the hole number from the
foundations doc and a few words; ctest inverts the result, so the case passes while the hole is
open and fails once it is fixed. That failure is the cue to delete the `xfail` line.

End `run` cases with `Report();` when they use `Res` or `Box`, and return 0 from `main`.
