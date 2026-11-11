#!/usr/bin/env python3
"""Wrap the interface's string literals in TR(): the text argument of
ui::text() and the format of snprintf(). Literals without two ASCII letters
in a row, already wrapped ones and log lines are left alone. Idempotent.

    python3 tools/i18n/wrap.py src/doba_ui.cpp ...
"""
import re
import sys

CALLS = {"ui::text(": 3, "snprintf(": 2}
LIT = re.compile(r'"(?:[^"\\]|\\.)*"')


def split_args(s, start):
    """Args of the call whose '(' is at start-1: [(begin, end)], end index."""
    depth, i, args, b = 0, start, [], start
    in_str = None
    while i < len(s):
        c = s[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == in_str:
                in_str = None
        elif c in "\"'":
            in_str = c
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                args.append((b, i))
                return args, i
            depth -= 1
        elif c == "," and depth == 0:
            args.append((b, i))
            b = i + 1
        i += 1
    return args, i


def wrap_arg(text):
    out, last = [], 0
    for m in LIT.finditer(text):
        lit = m.group(0)
        before = text[:m.start()].rstrip()
        bare = re.sub(r"%[-+ #0-9.*]*[hlLzjt]*[diouxXfFeEgGcsp%]", " ", lit)
        bare = re.sub(r"\\x[0-9A-Fa-f]{2}|\\.", " ", bare)
        if (before.endswith("TR(") or before.endswith("TRN(") or not re.search(r"[A-Za-z]{2}", bare)
                or "/" in lit and not " " in lit):        # paths, not words
            continue
        # adjacent literal pieces ("a" "b") stay one literal: skip if next is a literal
        out.append(text[last:m.start()])
        out.append("TR(" + lit + ")")
        last = m.end()
    out.append(text[last:])
    return "".join(out)


def process(s):
    res, i = [], 0
    pat = re.compile("|".join(re.escape(c) for c in CALLS))
    while True:
        m = pat.search(s, i)
        if not m:
            res.append(s[i:])
            return "".join(res)
        name = m.group(0)
        # not inside a comment line or a Serial/diag log
        line_start = s.rfind("\n", 0, m.start()) + 1
        line = s[line_start:m.start()]
        args, end = split_args(s, m.end())
        res.append(s[i:m.end()])
        k = CALLS[name]
        if "//" in line or len(args) <= k:
            res.append(s[m.end():end])
            i = end
            continue
        b, e = args[k]
        res.append(s[m.end():b])
        res.append(wrap_arg(s[b:e]))
        res.append(s[e:end])
        i = end


for p in sys.argv[1:]:
    s = open(p, encoding="utf-8").read()
    t = process(s)
    if t != s:
        if '#include "doba_i18n.h"' not in t:
            t = t.replace('#include "', '#include "doba_i18n.h"\n#include "', 1)
        open(p, "w", encoding="utf-8").write(t)
        print(p, t.count("TR(") - s.count("TR("), "wrapped")
