#!/usr/bin/env python
"""Roadmap phase 7: random programs over Owning and UserDrop types.

Each program is built from moves, copies, reassignments, optionals, arrays, branches, loops, early returns and
calls that take owned values. If the checker accepts a program it must run clean: every Res dropped exactly once
(END live=0), no double free from the poisoning allocator, no AddressSanitizer report, no read of freed memory
(-572662307), no panic and no crash. Programs the checker rejects only count toward the acceptance rate.

    python tests/fuzz/fuzz.py --stratac build/default/bin/stratac.exe --clang C:/llvm/bin/clang.exe \
        --count 500 --seed 1

Failing programs are kept in the output directory as fail_<seed>.strata with the observed output next to them.
"""
import argparse
import concurrent.futures
import glob
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PRELUDE = os.path.join(HERE, "..", "conformance", "prelude.strata")

HELPERS = """
struct S { ^Res a; ^Res b; };
int gSum = 0;
void Take(^Res r) { gSum = gSum + r.id; }
void Peek(const ref ^Res r) { gSum = gSum + r.id; }
^Res Pass(^Res r) { return r; }
"""


class Gen:
    def __init__(self, rng):
        self.r = rng
        self.n = 0
        self.lines = []
        self.depth = 0
        # scopes: list of dicts name -> (kind, live) ; kind in O (^Res), P (^Res?), A (^Res[])
        self.scopes = [{}]
        self.loop = 0

    def fresh(self, prefix):
        self.n += 1
        return f"{prefix}{self.n}"

    def emit(self, text):
        self.lines.append("    " * (self.depth + 1) + text)

    def vars(self, kind, live=None):
        out = []
        for scope in self.scopes:
            for name, (k, l) in scope.items():
                if k == kind and (live is None or l == live):
                    out.append(name)
        return out

    def set_live(self, name, live):
        for scope in reversed(self.scopes):
            if name in scope:
                scope[name] = (scope[name][0], live)
                return

    def declare(self, name, kind, live=True):
        self.scopes[-1][name] = (kind, live)

    def snapshot(self):
        return [dict(s) for s in self.scopes]

    def restore(self, snap):
        self.scopes = [dict(s) for s in snap]

    def merge(self, a, b):
        """Liveness after a branch: live only where live on both paths."""
        for sa, sb, s in zip(a, b, self.scopes):
            for name in list(s):
                la = sa.get(name, (None, False))[1]
                lb = sb.get(name, (None, False))[1]
                if isinstance(la, tuple) or isinstance(lb, tuple):
                    la = la if isinstance(la, tuple) else (False, False)
                    lb = lb if isinstance(lb, tuple) else (False, False)
                    s[name] = (s[name][0], (la[0] and lb[0], la[1] and lb[1]))
                else:
                    s[name] = (s[name][0], la and lb)

    def value(self):
        k = self.r.randint(1, 98)
        return f"Box({k})"

    # -- statements ---------------------------------------------------------------------------

    def stmt(self, budget):
        r = self.r
        choices = [
            self.decl_o, self.decl_o, self.decl_p, self.decl_a,
            self.move_o, self.take, self.peek, self.reassign_o, self.opt_assign,
            self.opt_use, self.array_push, self.array_remove, self.array_replace,
            self.pass_through, self.discard_temp,
            self.decl_s, self.decl_s, self.s_move_field, self.s_peek_field, self.s_set_field, self.s_dies,
        ]
        if budget > 0:
            choices += [self.branch, self.branch, self.loop_stmt, self.early_return, self.block]
        r.choice(choices)(budget - 1)

    def decl_o(self, b):
        name = self.fresh("v")
        self.emit(f"^Res {name} = {self.value()};")
        self.declare(name, "O")

    def move_o(self, b):
        live = self.vars("O", True)
        if not live:
            return
        src = self.r.choice(live)
        name = self.fresh("v")
        self.emit(f"^Res {name} = {src};")
        self.set_live(src, False)
        self.declare(name, "O")

    def take(self, b):
        live = self.vars("O", True)
        if not live:
            return
        src = self.r.choice(live)
        self.emit(f"Take({src});")
        self.set_live(src, False)

    def peek(self, b):
        live = self.vars("O", True)
        if not live:
            return
        self.emit(f"Peek({self.r.choice(live)});")

    def reassign_o(self, b):
        names = self.vars("O")
        if not names:
            return
        dst = self.r.choice(names)
        live = [v for v in self.vars("O", True) if v != dst]
        if live and self.r.random() < 0.4:
            src = self.r.choice(live)
            self.emit(f"{dst} = {src};")
            self.set_live(src, False)
        else:
            self.emit(f"{dst} = {self.value()};")
        self.set_live(dst, True)

    def decl_p(self, b):
        name = self.fresh("o")
        live = self.vars("O", True)
        if live and self.r.random() < 0.6:
            src = self.r.choice(live)
            self.emit(f"Res? {name} = {src};")
            self.set_live(src, False)
        elif self.r.random() < 0.5:
            self.emit(f"Res? {name} = {self.value()};")
        else:
            self.emit(f"Res? {name};")
        self.declare(name, "P")

    def opt_assign(self, b):
        names = self.vars("P")
        if not names:
            return
        dst = self.r.choice(names)
        live = self.vars("O", True)
        roll = self.r.random()
        if live and roll < 0.5:
            src = self.r.choice(live)
            self.emit(f"{dst} = {src};")
            self.set_live(src, False)
        elif roll < 0.8:
            self.emit(f"{dst} = {self.value()};")
        else:
            other = self.vars("P")
            src = self.r.choice(other)
            if src != dst:
                self.emit(f"{dst} = {src};")

    def opt_use(self, b):
        names = self.vars("P")
        if not names:
            return
        o = self.r.choice(names)
        t = self.fresh("v")
        self.emit(f"if ({o}?) {{")
        self.depth += 1
        mode = self.r.randint(0, 2)
        if mode == 0:
            self.emit(f"Peek({o});")
        elif mode == 1:
            self.emit(f"^Res {t} = {o};")
            self.emit(f"Take({t});")
        else:
            self.emit(f"^Res {t} = {o};")
            self.emit(f"Peek({t});")
        self.depth -= 1
        self.emit("}")

    def decl_a(self, b):
        name = self.fresh("a")
        self.emit(f"^Res[] {name} = {{}};")
        self.declare(name, "A")

    def array_push(self, b):
        names = self.vars("A")
        if not names:
            return
        a = self.r.choice(names)
        live = self.vars("O", True)
        if live and self.r.random() < 0.5:
            src = self.r.choice(live)
            self.emit(f"array_push({a}, {src});")
            self.set_live(src, False)
        else:
            self.emit(f"array_push({a}, {self.value()});")

    def array_remove(self, b):
        names = self.vars("A")
        if not names:
            return
        a = self.r.choice(names)
        fn = self.r.choice(["array_remove", "array_swap_remove"])
        self.emit(f"if ({a}.length > 0u) {{")
        self.depth += 1
        if self.r.random() < 0.5:
            t = self.fresh("v")
            self.emit(f"^Res {t} = {fn}({a}, 0u);")
            self.emit(f"Take({t});")
        else:
            self.emit(f"{fn}({a}, 0u);")
        self.depth -= 1
        self.emit("}")

    def array_replace(self, b):
        names = self.vars("A")
        if not names:
            return
        a = self.r.choice(names)
        t = self.fresh("v")
        self.emit(f"if ({a}.length > 0u) {{")
        self.depth += 1
        self.emit(f"^Res {t} = array_replace({a}, 0u, {self.value()});")
        self.emit(f"Peek({t});")
        self.depth -= 1
        self.emit("}")

    def pass_through(self, b):
        live = self.vars("O", True)
        if not live:
            return
        src = self.r.choice(live)
        name = self.fresh("v")
        self.emit(f"^Res {name} = Pass(Pass({src}));")
        self.set_live(src, False)
        self.declare(name, "O")

    def discard_temp(self, b):
        k = self.r.randint(0, 2)
        if k == 0:
            self.emit(f"Take({self.value()});")
        elif k == 1:
            self.emit(f"gSum = gSum + {self.value()}.id;")
        else:
            self.emit(f"Peek({self.value()});")

    def structs(self):
        out = []
        for scope in self.scopes:
            for name, (k, l) in scope.items():
                if k == "S":
                    out.append(name)
        return out

    def s_state(self, name):
        for scope in reversed(self.scopes):
            if name in scope:
                return scope[name][1]

    def s_put(self, name, state):
        for scope in reversed(self.scopes):
            if name in scope:
                scope[name] = ("S", state)
                return

    def decl_s(self, b):
        name = self.fresh("s")
        live = self.vars("O", True)
        if live and self.r.random() < 0.5:
            src = self.r.choice(live)
            self.emit(f"^S {name} = S {{ .a = {src}, .b = {self.value()} }};")
            self.set_live(src, False)
        else:
            self.emit(f"^S {name} = S {{ .a = {self.value()}, .b = {self.value()} }};")
        self.declare(name, "S", (True, True))

    def s_move_field(self, b):
        names = self.structs()
        if not names:
            return
        n = self.r.choice(names)
        st = self.s_state(n)
        f = self.r.randint(0, 1)
        if not st[f]:
            return
        t = self.fresh("v")
        self.emit(f"^Res {t} = {n}.{'ab'[f]};")
        self.s_put(n, tuple(False if i == f else st[i] for i in range(2)))
        self.declare(t, "O")

    def s_peek_field(self, b):
        names = self.structs()
        if not names:
            return
        n = self.r.choice(names)
        st = self.s_state(n)
        f = self.r.randint(0, 1)
        if st[f]:
            self.emit(f"Peek({n}.{'ab'[f]});")

    def s_set_field(self, b):
        names = self.structs()
        if not names:
            return
        n = self.r.choice(names)
        st = self.s_state(n)
        f = self.r.randint(0, 1)
        self.emit(f"{n}.{'ab'[f]} = {self.value()};")
        self.s_put(n, tuple(True if i == f else st[i] for i in range(2)))

    def s_dies(self, b):
        self.emit("defer { gSum = gSum + 1; }")

    # -- control flow -------------------------------------------------------------------------

    def cond(self):
        return self.r.choice(["c > 0", "c < 0", "c == 1", "c != 2", "(c & 1) == 1"])

    def sub_block(self, b, allow_return=False):
        self.scopes.append({})
        self.depth += 1
        for _ in range(self.r.randint(0, 4)):
            self.stmt(b)
        self.depth -= 1
        self.scopes.pop()

    def block(self, b):
        self.emit("{")
        self.sub_block(b)
        self.emit("}")

    def branch(self, b):
        before = self.snapshot()
        self.emit(f"if ({self.cond()}) {{")
        self.sub_block(b)
        arm_a = self.snapshot()
        self.restore(before)
        if self.r.random() < 0.6:
            self.emit("} else {")
            self.sub_block(b)
        arm_b = self.snapshot()
        self.emit("}")
        self.restore(before)
        self.merge(arm_a, arm_b)

    def loop_stmt(self, b):
        i = self.fresh("i")
        before = self.snapshot()
        self.emit(f"int {i} = 0;")
        self.emit(f"while ({i} < {self.r.randint(1, 3)}) {{")
        self.loop += 1
        self.scopes.append({})
        self.depth += 1
        for _ in range(self.r.randint(1, 3)):
            self.stmt(b)
        if self.r.random() < 0.25:
            self.emit(f"if ({self.cond()}) {{ {i}++; continue; }}")
        if self.r.random() < 0.2:
            self.emit(f"if ({self.cond()}) {{ break; }}")
        self.emit(f"{i}++;")
        self.depth -= 1
        self.scopes.pop()
        self.loop -= 1
        self.emit("}")
        # the body may not run, and may have moved: anything it touched is unknown afterwards
        after = self.snapshot()
        self.restore(before)
        self.merge(after, before)

    def early_return(self, b):
        before = self.snapshot()
        self.emit(f"if ({self.cond()}) {{")
        self.depth += 1
        if self.r.random() < 0.5:
            self.stmt(b)
        self.emit("return gSum;")
        self.depth -= 1
        self.emit("}")
        self.restore(before)

    def program(self):
        self.lines.append("int Run(int c)")
        self.lines.append("{")
        for _ in range(self.r.randint(3, 10)):
            self.stmt(3)
        self.lines.append("    return gSum;")
        self.lines.append("}")
        body = "\n".join(self.lines)
        main = """
int main()
{
    int total = 0;
    for (int c = -1; c < 4; c++)
    {
        total = total + Run(c);
        gSum = 0;
    }
    printf("sum %d\\n", total);
    Report();
    return 0;
}
"""
        return HELPERS + body + main


def run(cmd, env=None, timeout=60):
    return subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout)


def one(args, seed):
    rng = random.Random(seed)
    src = Gen(rng).program()
    work = os.path.join(args.out, f"s{seed}")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "p.strata")
    with open(PRELUDE, encoding="utf-8") as f:
        prelude = f.read()
    with open(path, "w", encoding="utf-8") as f:
        f.write(prelude + "\n" + src)

    obj = os.path.join(work, "p.o")
    exe = os.path.join(work, "p.exe")
    env = dict(os.environ, STRATA_ASAN="1")
    try:
        c = run([args.stratac, path, "-o", obj], env=env)
    except subprocess.TimeoutExpired:
        return seed, "FAIL", "stratac timed out", src
    if c.returncode != 0:
        text = c.stdout + c.stderr
        if "error" in text and c.returncode == 1:
            first = text.strip().splitlines()[0] if text.strip() else ""
            return seed, "reject", first.split("error:", 1)[-1].strip(), src
        return seed, "FAIL", f"stratac crashed ({c.returncode}): {text}", src

    link = run([args.clang, "--target=x86_64-pc-windows-msvc", "-fsanitize=address", obj, "-o", exe])
    if link.returncode != 0:
        return seed, "FAIL", "link failed: " + link.stderr, src

    renv = dict(os.environ, PATH=args.dll_dir + os.pathsep + os.environ["PATH"])
    try:
        p = run([exe], env=renv)
    except subprocess.TimeoutExpired:
        return seed, "FAIL", "program timed out", src
    log = p.stdout + p.stderr
    problems = []
    if "AddressSanitizer" in log:
        problems.append("AddressSanitizer report")
    if "!!! DOUBLE/BAD FREE" in log:
        problems.append("double or bad free")
    if "-572662307" in log:
        problems.append("read of freed memory")
    if "panic:" in log:
        problems.append("panic")
    if "END live=0" not in log:
        problems.append("leak or double drop: " + " ".join(l for l in log.splitlines() if "END live" in l))
    if p.returncode != 0 and not problems:
        problems.append(f"exit code {p.returncode}")
    if problems:
        return seed, "FAIL", "; ".join(problems) + "\n" + log[-1500:], src
    return seed, "ok", "", src


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stratac", required=True)
    ap.add_argument("--clang", required=True)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="fuzz-out")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--show-rejects", action="store_true")
    args = ap.parse_args()
    args.out = os.path.abspath(args.out)
    args.stratac = os.path.abspath(args.stratac)
    args.clang = os.path.abspath(args.clang)
    root = os.path.dirname(os.path.dirname(os.path.abspath(args.clang)))
    dirs = glob.glob(os.path.join(root, "lib", "clang", "*", "lib", "windows"))
    args.dll_dir = dirs[0] if dirs else os.path.dirname(args.clang)

    counts = {"ok": 0, "reject": 0, "FAIL": 0}
    rejects = {}
    seeds = range(args.seed, args.seed + args.count)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for seed, status, detail, src in pool.map(lambda s: one(args, s), seeds):
            counts[status] += 1
            if status == "FAIL":
                print(f"seed {seed}: FAIL {detail}")
                with open(os.path.join(args.out, f"fail_{seed}.strata"), "w", encoding="utf-8") as f:
                    f.write(src)
            elif status == "reject":
                rejects[detail] = rejects.get(detail, 0) + 1
    print(f"programs {args.count}: ok {counts['ok']}, rejected {counts['reject']}, failed {counts['FAIL']}")
    if args.show_rejects:
        for text, n in sorted(rejects.items(), key=lambda kv: -kv[1])[:15]:
            print(f"  {n:4d}  {text}")
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
