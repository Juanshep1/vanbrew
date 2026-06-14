#!/usr/bin/env python3
"""
Vanta - a real, plain-English programming language.

Vanta reads like English but is a genuine general-purpose language. It has
variables, arithmetic, the full set of control flow, functions with default
arguments and recursion, first-class and higher-order functions, user-defined
types with methods, error handling, lists and maps, string interpolation,
modules, file and system access, and a sizeable standard library.

The whole language is this one file, built in the three classic stages:

  1. TOKENIZE  - chop text into little pieces (words, numbers, symbols)
  2. PARSE     - turn those pieces into a structure (an "abstract syntax tree")
  3. RUN       - walk that structure and actually do what it says

Run a file:   python3 vanta.py program.va
Start REPL:   python3 vanta.py
"""

import sys
import os
import re
import shutil
import json
import time
import math
import subprocess
import random

VERSION = "4.2"

# Command-line arguments passed to a Vanta program (after the script name).
PROGRAM_ARGS = []


class VantaError(Exception):
    """A friendly error in a Vanta program."""


# ===========================================================================
# STAGE 1 - TOKENIZER
# ===========================================================================

# English phrases that mean the same as a symbol.
PHRASES = {
    ("is", "at", "least"): ">=",
    ("is", "at", "most"): "<=",
    ("is", "greater", "than"): ">",
    ("is", "bigger", "than"): ">",
    ("is", "more", "than"): ">",
    ("is", "less", "than"): "<",
    ("is", "smaller", "than"): "<",
    ("is", "not", "in"): "notin",
    ("is", "not"): "!=",
    ("is", "a"): "isa",
    ("is", "an"): "isa",
    ("is", "in"): "in",
    ("is", "over"): ">",
    ("is", "above"): ">",
    ("is", "under"): "<",
    ("is", "below"): "<",
    ("divided", "by"): "/",
    ("is",): "==",
    ("plus",): "+",
    ("minus",): "-",
    ("times",): "*",
}

COMPARE_OPS = {">", "<", ">=", "<=", "==", "!="}


def tokenize(text):
    """Turn a line of source into a list of (kind, value) tokens."""
    tokens = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]

        if c in " \t":
            i += 1
            continue

        if c == "#":            # rest of the line is a comment
            break

        if c == '"':
            j = i + 1
            chars = []
            escapes = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    chars.append(escapes.get(text[j + 1], text[j + 1]))
                    j += 2
                else:
                    chars.append(text[j])
                    j += 1
            if j >= n:
                raise VantaError('a text value is missing its closing quote (")')
            tokens.append(("STR", "".join(chars)))
            i = j + 1
            continue

        if c.isdigit():
            j, dot = i, False
            while j < n and (text[j].isdigit() or (text[j] == "." and not dot)):
                dot = dot or text[j] == "."
                j += 1
            chunk = text[i:j]
            tokens.append(("NUM", float(chunk) if dot else int(chunk)))
            i = j
            continue

        if c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            tokens.append(("NAME", text[i:j]))
            i = j
            continue

        two = text[i:i + 2]
        if two in (">=", "<=", "==", "!="):
            tokens.append(("OP", two))
            i += 2
            continue
        if c in "+-*/%><^":
            tokens.append(("OP", c))
            i += 1
            continue
        if c == "=":
            tokens.append(("OP", "=="))
            i += 1
            continue
        simple = {"(": "LP", ")": "RP", "[": "LB", "]": "RB", "{": "LC",
                  "}": "RC", ",": "COMMA", ":": "COLON", ".": "DOT"}
        if c in simple:
            tokens.append((simple[c], c))
            i += 1
            continue

        raise VantaError(f"I don't understand the symbol: {c}")

    return combine_word_operators(tokens)


def combine_word_operators(tokens):
    """Fold word-phrases like ['is','at','least'] into a single OP token."""
    out, i = [], 0
    while i < len(tokens):
        if tokens[i][0] == "NAME":
            words, k = [], i
            while k < len(tokens) and tokens[k][0] == "NAME" and len(words) < 3:
                words.append(tokens[k][1].lower())
                k += 1
            matched = False
            for size in (3, 2, 1):
                phrase = tuple(words[:size])
                if len(phrase) == size and phrase in PHRASES:
                    out.append(("OP", PHRASES[phrase]))
                    i += size
                    matched = True
                    break
            if matched:
                continue
        out.append(tokens[i])
        i += 1
    return out


# ===========================================================================
# STAGE 2 - EXPRESSION PARSER  (text -> tree)
# ===========================================================================
#
# Precedence, lowest to highest:
#   or -> and -> not -> comparison -> + - -> * / % -> unary - / new -> ^
#   -> call() / index[] / .attr -> primary

class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0

    def at_end(self):
        return self.pos >= len(self.tokens)

    def peek(self):
        return self.tokens[self.pos] if self.pos < len(self.tokens) else ("END", None)

    def take(self):
        tok = self.peek()
        self.pos += 1
        return tok

    def is_word(self, word):
        k, v = self.peek()
        return k == "NAME" and v.lower() == word

    def parse_expression(self):
        node = self.parse_or()
        if self.is_word("if"):                 # inline conditional: A if COND otherwise B
            self.take()
            cond = self.parse_or()
            if not self.is_word("otherwise"):
                raise VantaError("an inline 'if' needs an 'otherwise' value")
            self.take()
            return ("ternary", cond, node, self.parse_expression())
        return node

    def parse_or(self):
        node = self.parse_and()
        while self.is_word("or"):
            self.take()
            node = ("or", node, self.parse_and())
        return node

    def parse_and(self):
        node = self.parse_not()
        while self.is_word("and"):
            self.take()
            node = ("and", node, self.parse_not())
        return node

    def parse_not(self):
        if self.is_word("not"):
            self.take()
            return ("not", self.parse_not())
        return self.parse_comparison()

    def parse_comparison(self):
        left = self.parse_add_sub()
        k, v = self.peek()
        if k == "OP" and v == "isa":
            self.take()
            return ("isa", left, self.parse_add_sub())
        if k == "OP" and v in ("in", "notin"):
            self.take()
            return (v, left, self.parse_add_sub())
        if k == "OP" and v in COMPARE_OPS:
            self.take()
            return ("cmp", v, left, self.parse_add_sub())
        return left

    def parse_add_sub(self):
        node = self.parse_mul_div()
        while self.peek()[0] == "OP" and self.peek()[1] in ("+", "-"):
            op = self.take()[1]
            node = ("arith", op, node, self.parse_mul_div())
        return node

    def parse_mul_div(self):
        node = self.parse_unary()
        while self.peek()[0] == "OP" and self.peek()[1] in ("*", "/", "%"):
            op = self.take()[1]
            node = ("arith", op, node, self.parse_unary())
        return node

    def parse_unary(self):
        if self.peek() == ("OP", "-"):
            self.take()
            return ("neg", self.parse_unary())
        if self.is_word("make"):          # anonymous function: make x give x * 2
            return self.parse_lambda()
        if self.is_word("new"):           # "new Dog(...)" is sugar for "Dog(...)"
            self.take()
            return self.parse_power()
        return self.parse_power()

    def parse_lambda(self):
        self.take()                       # 'make'
        params = []
        while not self.is_word("give"):
            k, v = self.take()
            if k == "NAME":
                params.append(v)
            elif k != "COMMA":
                raise VantaError("a 'make' function reads: make PARAMS give VALUE")
        self.take()                       # 'give'
        return ("lambda", params, self.parse_expression())

    def parse_power(self):
        base = self.parse_postfix()
        if self.peek() == ("OP", "^"):
            self.take()
            return ("arith", "^", base, self.parse_unary())   # right-associative
        return base

    def parse_postfix(self):
        node = self.parse_primary()
        while True:
            kind = self.peek()[0]
            if kind == "LP":
                self.take()
                node = ("call", node, self.parse_list_until("RP"))
            elif kind == "LB":
                self.take()
                start = None if self.peek()[0] == "COLON" else self.parse_expression()
                if self.peek()[0] == "COLON":          # a slice: [a:b], [a:], [:b], [:]
                    self.take()
                    end = None if self.peek()[0] == "RB" else self.parse_expression()
                    self.expect("RB", "a closing ] is missing")
                    node = ("sliceop", node, start, end)
                else:
                    self.expect("RB", "a closing ] is missing")
                    node = ("index", node, start)
            elif kind == "DOT":
                self.take()
                k, v = self.take()
                if k != "NAME":
                    raise VantaError("expected a name after '.'")
                node = ("getattr", node, v)
            else:
                return node

    def parse_primary(self):
        k, v = self.take()
        if k == "NUM":
            return ("lit", v)
        if k == "STR":
            return build_string_node(v)
        if k == "NAME":
            low = v.lower()
            if low in ("yes", "true"):
                return ("lit", True)
            if low in ("no", "false"):
                return ("lit", False)
            if low == "nothing":
                return ("lit", None)
            return ("name", v)
        if k == "LP":
            node = self.parse_expression()
            self.expect("RP", "a closing ) is missing")
            return node
        if k == "LB":
            return self.parse_list_or_comprehension()
        if k == "LC":
            return self.parse_map_or_comprehension()
        raise VantaError("that line is missing a value somewhere")

    def parse_list_or_comprehension(self):
        if self.peek()[0] == "RB":
            self.take()
            return ("list", [])
        first = self.parse_expression()
        if self.is_word("for"):                 # [EXPR for each NAME in LIST if COND]
            self.take()
            if not self.is_word("each"):
                raise VantaError("a comprehension reads [VALUE for each NAME in LIST]")
            self.take()
            k, name = self.take()
            if k != "NAME":
                raise VantaError("expected a name after 'for each'")
            if not self.is_word("in"):
                raise VantaError("a comprehension needs 'in' before the list")
            self.take()
            iter_expr = self.parse_or()    # not parse_expression: leave the filter 'if' alone
            cond = None
            if self.is_word("if"):
                self.take()
                cond = self.parse_expression()
            self.expect("RB", "a closing ] is missing")
            return ("listcomp", first, name, iter_expr, cond)
        items = [first]
        while True:
            k = self.take()[0]
            if k == "RB":
                return ("list", items)
            if k != "COMMA":
                raise VantaError("expected a comma between values")
            items.append(self.parse_expression())

    def parse_list_until(self, closer):
        items = []
        if self.peek()[0] == closer:
            self.take()
            return items
        while True:
            items.append(self.parse_expression())
            k = self.take()[0]
            if k == closer:
                return items
            if k != "COMMA":
                raise VantaError("expected a comma between values")

    def parse_map_or_comprehension(self):
        if self.peek()[0] == "RC":
            self.take()
            return ("map", [])
        key = self.parse_expression()
        self.expect("COLON", "a map needs a : between key and value")
        value = self.parse_expression()
        if self.is_word("for"):              # {KEY: VALUE for each NAME in LIST if COND}
            self.take()
            if not self.is_word("each"):
                raise VantaError("a map comprehension reads {KEY: VALUE for each NAME in LIST}")
            self.take()
            k, name = self.take()
            if k != "NAME":
                raise VantaError("expected a name after 'for each'")
            if not self.is_word("in"):
                raise VantaError("a comprehension needs 'in' before the list")
            self.take()
            iter_expr = self.parse_or()    # not parse_expression: leave the filter 'if' alone
            cond = None
            if self.is_word("if"):
                self.take()
                cond = self.parse_expression()
            self.expect("RC", "a closing } is missing")
            return ("mapcomp", key, value, name, iter_expr, cond)
        pairs = [(key, value)]
        while True:
            k = self.take()[0]
            if k == "RC":
                return ("map", pairs)
            if k != "COMMA":
                raise VantaError("expected a comma between map entries")
            key = self.parse_expression()
            self.expect("COLON", "a map needs a : between key and value")
            value = self.parse_expression()
            pairs.append((key, value))

    def expect(self, kind, message):
        if self.take()[0] != kind:
            raise VantaError(message)


def build_string_node(text):
    """Turn a string that may contain {expressions} into either a plain
    literal or a 'format' node that concatenates pieces at runtime.
    Use {{ and }} for literal braces."""
    if "{" not in text and "}" not in text:
        return ("lit", text)
    parts, buf = [], []
    has_expr = False
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == "{" and i + 1 < n and text[i + 1] == "{":
            buf.append("{")
            i += 2
        elif ch == "}" and i + 1 < n and text[i + 1] == "}":
            buf.append("}")
            i += 2
        elif ch == "}":
            raise VantaError("a '}' in text has no matching '{' (use '}}' for a literal)")
        elif ch == "{":
            parts.append(("lit", "".join(buf)))
            buf = []
            j = i + 1
            while j < n and text[j] != "}":
                j += 1
            if j >= n:
                raise VantaError("a '{' in text is missing its '}' (use '{{' for a literal)")
            src = text[i + 1:j].strip()
            if not src:
                raise VantaError("there is an empty {} in text")
            sub = Parser(tokenize(src))
            node = sub.parse_expression()
            if not sub.at_end():
                raise VantaError("I got confused inside { } in text")
            parts.append(node)
            has_expr = True
            i = j + 1
        else:
            buf.append(ch)
            i += 1
    parts.append(("lit", "".join(buf)))
    if not has_expr:
        return ("lit", "".join(p[1] for p in parts))
    return ("format", parts)


def parse_expr_text(text, lineno):
    try:
        parser = Parser(tokenize(text))
        node = parser.parse_expression()
        if not parser.at_end():
            raise VantaError("I got confused near the end of that line")
        return node
    except VantaError as e:
        msg = str(e)
        raise VantaError(msg if msg.startswith("line ") else f"line {lineno}: {msg}")


# ===========================================================================
# STAGE 2b - STATEMENT PARSER  (lines -> list of statements)
# ===========================================================================

BLOCK_TERMINATORS = ("end", "otherwise", "rescue", "when")
BLOCK_OPENERS = ("if", "repeat", "while", "for", "to", "attempt", "type", "match")


def first_word(line):
    return line.split(" ", 1)[0] if line else ""


def split_top_level(text, sep):
    """Split on sep, but not inside (), [], {} or "strings"."""
    parts, cur, depth, in_str = [], [], 0, False
    for c in text:
        if in_str:
            cur.append(c)
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            cur.append(c)
        elif c in "([{":
            depth += 1
            cur.append(c)
        elif c in ")]}":
            depth -= 1
            cur.append(c)
        elif c == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    parts.append("".join(cur))
    return parts


def parse_program(lines):
    stmts, pos = parse_block(lines, 0)
    if pos != len(lines):
        raise VantaError(f"line {lines[pos][0]}: an extra "
                         f"'{first_word(lines[pos][1])}' has no matching block")
    return stmts


def parse_block(lines, pos):
    stmts = []
    while pos < len(lines):
        if first_word(lines[pos][1]) in BLOCK_TERMINATORS:
            return stmts, pos
        stmt, pos = parse_one(lines, pos)
        stmts.append(stmt)
    return stmts, pos


def parse_one(lines, pos):
    lineno, text = lines[pos]
    head = first_word(text)
    rest = text[len(head):].strip()

    if head == "if":
        return parse_if(lines, pos)

    if head == "repeat":
        count_text = rest[:-5].strip() if rest.endswith("times") else rest
        body, pos = parse_block(lines, pos + 1)
        pos = expect_end(lines, pos, "repeat")
        return ("repeat", lineno, parse_expr_text(count_text, lineno), body), pos

    if head == "while":
        body, pos = parse_block(lines, pos + 1)
        pos = expect_end(lines, pos, "while")
        return ("while", lineno, parse_expr_text(rest, lineno), body), pos

    if head == "for":
        if not rest.startswith("each "):
            raise VantaError(f"line {lineno}: use: for each NAME in LIST")
        after = rest[len("each "):]
        if " in " not in after:
            raise VantaError(f"line {lineno}: use: for each NAME in LIST")
        names_text, _, iter_text = after.partition(" in ")
        names = [n.strip() for n in names_text.split(",")]
        for nm in names:
            check_name(nm, lineno)
        body, pos = parse_block(lines, pos + 1)
        pos = expect_end(lines, pos, "for")
        if len(names) == 1:
            return ("foreach", lineno, names[0],
                    parse_expr_text(iter_text, lineno), body), pos
        if len(names) == 2:
            return ("foreach2", lineno, names[0], names[1],
                    parse_expr_text(iter_text, lineno), body), pos
        raise VantaError(f"line {lineno}: 'for each' takes one or two names")

    if head == "increase" or head == "decrease":
        if " by " not in rest:
            raise VantaError(f"line {lineno}: use: {head} NAME by AMOUNT")
        target, _, amount_text = rest.partition(" by ")
        sign = "+" if head == "increase" else "-"
        target_node = parse_expr_text(target.strip(), lineno)
        amount = parse_expr_text(amount_text, lineno)
        return ("mutate", lineno, target_node, sign, amount), pos + 1

    if head == "match":
        return parse_match(lines, pos)

    if head == "to":
        name, params = parse_signature(rest, lineno)
        body, pos = parse_block(lines, pos + 1)
        pos = expect_end(lines, pos, "to")
        return ("func", lineno, name, params, body), pos

    if head == "type":
        return parse_type(lines, pos)

    if head == "attempt":
        return parse_attempt(lines, pos)

    if head == "give":
        if not rest.startswith("back"):
            raise VantaError(f"line {lineno}: did you mean 'give back ...'?")
        value_text = rest[len("back"):].strip()
        node = parse_expr_text(value_text, lineno) if value_text else ("lit", None)
        return ("return", lineno, node), pos + 1

    if head == "let":
        name_part, sep, expr_text = rest.partition(" be ")
        if not sep:
            raise VantaError(f"line {lineno}: use: let NAME be VALUE")
        names = [t.strip() for t in split_top_level(name_part, ",")]
        if len(names) > 1:
            for nm in names:
                check_name(nm, lineno)
            value_nodes = [parse_expr_text(s.strip(), lineno)
                           for s in split_top_level(expr_text, ",")]
            return ("let_multi", lineno, names, value_nodes), pos + 1
        check_name(names[0], lineno)
        return ("let", lineno, names[0], parse_expr_text(expr_text, lineno)), pos + 1

    if head == "fix":
        name, sep, expr_text = rest.partition(" be ")
        if not sep:
            raise VantaError(f"line {lineno}: use: fix NAME be VALUE")
        check_name(name.strip(), lineno)
        return ("fix", lineno, name.strip(), parse_expr_text(expr_text, lineno)), pos + 1

    if head == "change":
        if " to " not in rest:
            raise VantaError(f"line {lineno}: use: change NAME to VALUE")
        target, _, value_text = rest.partition(" to ")
        target = target.strip()
        target_segs = split_top_level(target, ",")
        if len(target_segs) > 1:
            target_nodes = [parse_expr_text(s.strip(), lineno) for s in target_segs]
            value_nodes = [parse_expr_text(s.strip(), lineno)
                           for s in split_top_level(value_text, ",")]
            return ("assign_multi", lineno, target_nodes, value_nodes), pos + 1
        value = parse_expr_text(value_text, lineno)
        if " at " in target and "[" not in target and "." not in target:
            name, _, index_text = target.partition(" at ")
            target_node = ("index", ("name", name.strip()),
                           parse_expr_text(index_text, lineno))
        else:
            target_node = parse_expr_text(target, lineno)
        return ("assign", lineno, target_node, value), pos + 1

    if head == "ask":
        if " into " not in rest:
            raise VantaError(f'line {lineno}: use: ask "question" into NAME')
        prompt_text, name = rest.rsplit(" into ", 1)
        check_name(name.strip(), lineno)
        prompt = parse_expr_text(prompt_text, lineno) if prompt_text.strip() else None
        return ("ask", lineno, prompt, name.strip()), pos + 1

    if head == "add" and " to " in rest:
        value_text, sep, list_text = rest.rpartition(" to ")
        return ("append", lineno, parse_expr_text(value_text, lineno),
                parse_expr_text(list_text, lineno)), pos + 1

    if head == "import":
        return ("import", lineno, parse_expr_text(rest, lineno)), pos + 1

    if head == "say":
        node = parse_expr_text(rest, lineno) if rest else None
        return ("say", lineno, node), pos + 1

    if head == "stop":
        return ("stop", lineno), pos + 1

    if head == "skip":
        return ("skip", lineno), pos + 1

    # Anything else is a bare expression (typically a function or method call).
    return ("expr", lineno, parse_expr_text(text, lineno)), pos + 1


def parse_if(lines, pos):
    lineno, text = lines[pos]
    branches = []
    cond = parse_expr_text(text[len("if"):].strip(), lineno)
    body, pos = parse_block(lines, pos + 1)
    branches.append((cond, body))
    else_body = []
    while pos < len(lines) and first_word(lines[pos][1]) == "otherwise":
        after = lines[pos][1][len("otherwise"):].strip()
        if after.startswith("if"):
            cond2 = parse_expr_text(after[len("if"):].strip(), lines[pos][0])
            body2, pos = parse_block(lines, pos + 1)
            branches.append((cond2, body2))
        else:
            else_body, pos = parse_block(lines, pos + 1)
            break
    pos = expect_end(lines, pos, "if")
    return ("if", lineno, branches, else_body), pos


def parse_match(lines, pos):
    lineno, text = lines[pos]
    subject = parse_expr_text(text[len("match"):].strip(), lineno)
    branches, else_body = [], []
    pos += 1
    while pos < len(lines):
        head = first_word(lines[pos][1])
        if head == "when":
            value = parse_expr_text(lines[pos][1][len("when"):].strip(), lines[pos][0])
            body, pos = parse_block(lines, pos + 1)
            branches.append((value, body))
        elif head == "otherwise":
            else_body, pos = parse_block(lines, pos + 1)
            break
        elif head == "end":
            break
        else:
            raise VantaError(f"line {lines[pos][0]}: inside 'match', "
                             f"use 'when VALUE' or 'otherwise'")
    pos = expect_end(lines, pos, "match")
    return ("match", lineno, subject, branches, else_body), pos


def parse_type(lines, pos):
    lineno, text = lines[pos]
    header = text[len("type"):].strip()
    parent_name = None
    if " from " in header:
        type_name, _, parent_name = header.partition(" from ")
        type_name, parent_name = type_name.strip(), parent_name.strip()
        check_name(parent_name, lineno)
    else:
        type_name = header
    check_name(type_name, lineno)
    fields, methods = [], {}
    pos += 1
    while pos < len(lines) and first_word(lines[pos][1]) != "end":
        l2, t2 = lines[pos]
        h2 = first_word(t2)
        if h2 == "has":
            field = t2[len("has"):].strip()
            check_name(field, l2)
            fields.append(field)
            pos += 1
        elif h2 == "to":
            mstmt, pos = parse_one(lines, pos)
            methods[mstmt[2]] = Function(mstmt[2], mstmt[3], mstmt[4])
        else:
            raise VantaError(f"line {l2}: inside a type, use 'has NAME' or 'to METHOD()'")
    pos = expect_end(lines, pos, "type")
    return ("type", lineno, type_name, parent_name, fields, methods), pos


def parse_attempt(lines, pos):
    lineno = lines[pos][0]
    body, pos = parse_block(lines, pos + 1)
    if pos >= len(lines) or first_word(lines[pos][1]) != "rescue":
        raise VantaError(f"line {lineno}: 'attempt' needs a 'rescue NAME' part")
    errname = lines[pos][1][len("rescue"):].strip() or "error"
    check_name(errname, lines[pos][0])
    rescue_body, pos = parse_block(lines, pos + 1)
    pos = expect_end(lines, pos, "attempt")
    return ("attempt", lineno, body, errname, rescue_body), pos


def parse_signature(sig, lineno):
    if "(" in sig and sig.endswith(")"):
        name = sig[:sig.index("(")].strip()
        inside = sig[sig.index("(") + 1:-1].strip()
        param_texts = [p.strip() for p in inside.split(",") if p.strip()]
    elif " with " in sig:
        name, _, rest = sig.partition(" with ")
        param_texts = [p.strip() for p in rest.split(",") if p.strip()]
    else:
        name, param_texts = sig.strip(), []
    check_name(name, lineno)
    params = []
    seen_default = False
    for p in param_texts:
        if " be " in p:
            pname, _, dtext = p.partition(" be ")
            pname = pname.strip()
            check_name(pname, lineno)
            params.append((pname, parse_expr_text(dtext.strip(), lineno)))
            seen_default = True
        else:
            check_name(p, lineno)
            if seen_default:
                raise VantaError(f"line {lineno}: '{p}' has no default but comes "
                                 f"after one that does")
            params.append((p, None))
    return name, params


def expect_end(lines, pos, what):
    if pos >= len(lines) or first_word(lines[pos][1]) != "end":
        raise VantaError(f"your '{what}' is missing its 'end'")
    return pos + 1


# ===========================================================================
# STAGE 3 - RUNTIME
# ===========================================================================

class Function:
    def __init__(self, name, params, body):
        self.name = name
        self.params = params          # list of (name, default_node_or_None)
        self.body = body
        self.compiled = None          # compiled body (a block runner); built on demand
        self.owner = None             # the type a method was defined in (for super)
        self.defining_env = None      # the scope it was defined in (for closures)


class Builtin:
    def __init__(self, name, fn):
        self.name = name
        self.fn = fn


class VantaType:
    def __init__(self, name, fields, methods, parent=None):
        self.name = name
        self.fields = fields
        self.methods = methods
        self.parent = parent


class VantaInstance:
    def __init__(self, vtype, attrs):
        self.vtype = vtype
        self.attrs = attrs


class BoundMethod:
    def __init__(self, inst, fn):
        self.inst = inst
        self.fn = fn


class SuperProxy:
    """What 'super' resolves to inside a method: lets you call the parent's
    version of a method, bound to the same object."""
    def __init__(self, inst, parent_type):
        self.inst = inst
        self.parent_type = parent_type


_MISSING = object()


class Environment:
    # `consts` (names fixed with 'fix') defaults to a shared empty frozenset, so
    # the common environment — a function call frame — costs nothing extra. The
    # first 'fix' in a scope upgrades it to a real, mutable set.
    consts = frozenset()

    def __init__(self, parent=None):
        self.vars = {}
        self.parent = parent

    def get(self, name):
        env = self
        while env is not None:
            if name in env.vars:
                return env.vars[name]
            env = env.parent
        return _MISSING

    def define(self, name, value):
        self.vars[name] = value

    def fix(self, name, value):
        self.vars[name] = value
        if type(self.consts) is frozenset:
            self.consts = set()
        self.consts.add(name)

    def assign(self, name, value):
        env = self
        while env is not None:
            if name in env.vars:
                if name in env.consts:
                    return "const"
                env.vars[name] = value
                return True
            env = env.parent
        return False


class ReturnSignal(Exception):
    def __init__(self, value):
        self.value = value


class BreakLoop(Exception):
    pass


class ContinueLoop(Exception):
    pass


def run_block(stmts, env):
    # The per-statement try/except attaches a line number to runtime errors.
    # Inlined here (rather than via a run_stmt helper) to avoid a function call
    # on every statement — this is one of the hottest loops in the interpreter.
    for stmt in stmts:
        try:
            _run_stmt(stmt, env)
        except VantaError as e:
            if str(e).startswith("line "):
                raise
            raise VantaError(f"line {stmt[1]}: {e}")


def run_stmt(stmt, env):
    try:
        _run_stmt(stmt, env)
    except VantaError as e:
        if str(e).startswith("line "):
            raise
        raise VantaError(f"line {stmt[1]}: {e}")


def _run_stmt(stmt, env):
    tag = stmt[0]

    # --- hot statement types first (kept near the top for dispatch speed) ---

    if tag == "if":
        _, _, branches, else_body = stmt
        for cond, body in branches:
            if truthy(eval_expr(cond, env)):
                run_block(body, env)
                return
        run_block(else_body, env)

    elif tag == "return":
        raise ReturnSignal(eval_expr(stmt[2], env))

    elif tag == "assign":
        do_assign(stmt[2], eval_expr(stmt[3], env), env)

    elif tag == "let":
        env.define(stmt[2], eval_expr(stmt[3], env))

    elif tag == "expr":
        eval_expr(stmt[2], env)

    elif tag == "mutate":
        _, _, target_node, sign, amount_node = stmt
        current = eval_expr(target_node, env)
        new_value = arithmetic(sign, current, eval_expr(amount_node, env))
        do_assign(target_node, new_value, env)

    elif tag == "say":
        node = stmt[2]
        print("" if node is None else display(eval_expr(node, env)))

    # --- everything else ---

    elif tag == "fix":
        env.fix(stmt[2], eval_expr(stmt[3], env))

    elif tag == "let_multi":
        names, value_nodes = stmt[2], stmt[3]
        for nm, val in zip(names, eval_value_group(value_nodes, len(names), env)):
            env.define(nm, val)

    elif tag == "assign_multi":
        targets, value_nodes = stmt[2], stmt[3]
        for tnode, val in zip(targets, eval_value_group(value_nodes, len(targets), env)):
            do_assign(tnode, val, env)

    elif tag == "append":
        value = eval_expr(stmt[2], env)
        target = eval_expr(stmt[3], env)
        if not isinstance(target, list):
            raise VantaError("you can only 'add ... to' a list")
        target.append(value)

    elif tag == "ask":
        prompt = "" if stmt[2] is None else display(eval_expr(stmt[2], env))
        answer = input(prompt + " " if prompt else "")
        env.define(stmt[3], smart_value(answer))

    elif tag == "repeat":
        count = eval_expr(stmt[2], env)
        if not isinstance(count, int) or isinstance(count, bool):
            raise VantaError("repeat needs a whole number of times")
        for _ in range(count):
            try:
                run_block(stmt[3], env)
            except BreakLoop:
                break
            except ContinueLoop:
                continue

    elif tag == "while":
        while truthy(eval_expr(stmt[2], env)):
            try:
                run_block(stmt[3], env)
            except BreakLoop:
                break
            except ContinueLoop:
                continue

    elif tag == "foreach":
        _, _, name, iter_node, body = stmt
        for item in iterate(eval_expr(iter_node, env)):
            env.define(name, item)
            try:
                run_block(body, env)
            except BreakLoop:
                break
            except ContinueLoop:
                continue

    elif tag == "foreach2":
        _, _, name1, name2, iter_node, body = stmt
        for first, second in iterate_pairs(eval_expr(iter_node, env)):
            env.define(name1, first)
            env.define(name2, second)
            try:
                run_block(body, env)
            except BreakLoop:
                break
            except ContinueLoop:
                continue

    elif tag == "match":
        _, _, subject_node, branches, else_body = stmt
        subject = eval_expr(subject_node, env)
        for value_node, body in branches:
            if subject == eval_expr(value_node, env):
                run_block(body, env)
                return
        run_block(else_body, env)

    elif tag == "func":
        _, _, name, params, body = stmt
        fn = Function(name, params, body)
        fn.defining_env = env          # so a nested function closes over this scope
        env.define(name, fn)

    elif tag == "type":
        _, _, name, parent_name, fields, own_methods = stmt
        parent = None
        final_fields = list(fields)
        final_methods = dict(own_methods)
        if parent_name is not None:
            parent = env.get(parent_name)
            if not isinstance(parent, VantaType):
                raise VantaError(f"'{parent_name}' is not a type to inherit from")
            final_fields = list(parent.fields)
            for field in fields:
                if field not in final_fields:
                    final_fields.append(field)
            final_methods = dict(parent.methods)
            final_methods.update(own_methods)
        vtype = VantaType(name, final_fields, final_methods, parent)
        for method in own_methods.values():     # remember where each was defined
            method.owner = vtype
        env.define(name, vtype)

    elif tag == "attempt":
        _, _, body, errname, rescue_body = stmt
        try:
            run_block(body, env)
        except VantaError as e:
            env.define(errname, clean_message(str(e)))
            run_block(rescue_body, env)

    elif tag == "stop":
        raise BreakLoop()

    elif tag == "skip":
        raise ContinueLoop()

    elif tag == "import":
        import_file(display(eval_expr(stmt[2], env)))

    else:
        raise VantaError(f"internal error: unknown statement {tag}")


def clean_message(msg):
    """Drop a leading 'line N: ' so rescue gets a tidy message."""
    if msg.startswith("line "):
        idx = msg.find(": ")
        if idx != -1:
            return msg[idx + 2:]
    return msg


# --------------------------------------------------------------------------
# Expression evaluation
# --------------------------------------------------------------------------

def eval_expr(node, env):
    tag = node[0]

    if tag == "lit":
        return node[1]

    if tag == "name":
        value = env.get(node[1])
        if value is not _MISSING:
            return value
        if node[1] in BUILTIN_VALUES:
            return BUILTIN_VALUES[node[1]]
        raise VantaError(f"I don't know what '{node[1]}' is yet")

    # --- hot node types first (kept near the top for dispatch speed) ---

    if tag == "arith":
        return arithmetic(node[1], eval_expr(node[2], env), eval_expr(node[3], env))

    if tag == "cmp":
        return compare(node[1], eval_expr(node[2], env), eval_expr(node[3], env))

    if tag == "call":
        callee = resolve_callee(node[1], env)
        args = [eval_expr(a, env) for a in node[2]]
        return apply_callable(callee, args)

    if tag == "index":
        return get_index(eval_expr(node[1], env), eval_expr(node[2], env))

    if tag == "getattr":
        return get_attr(eval_expr(node[1], env), node[2])

    # --- everything else ---

    if tag == "format":
        return "".join(display(eval_expr(part, env)) for part in node[1])

    if tag == "list":
        return [eval_expr(item, env) for item in node[1]]

    if tag == "map":
        result = {}
        for key_node, val_node in node[1]:
            result[eval_expr(key_node, env)] = eval_expr(val_node, env)
        return result

    if tag == "neg":
        value = eval_expr(node[1], env)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise VantaError("only numbers can be negative")
        return -value

    if tag == "not":
        return not truthy(eval_expr(node[1], env))

    if tag == "and":
        if not truthy(eval_expr(node[1], env)):
            return False
        return truthy(eval_expr(node[2], env))

    if tag == "or":
        if truthy(eval_expr(node[1], env)):
            return True
        return truthy(eval_expr(node[2], env))

    if tag == "isa":
        target = eval_expr(node[2], env)
        if not isinstance(target, VantaType):
            raise VantaError("the right side of 'is a' must be a type")
        return is_instance_of(eval_expr(node[1], env), target)

    if tag == "ternary":
        return eval_expr(node[2], env) if truthy(eval_expr(node[1], env)) \
            else eval_expr(node[3], env)

    if tag == "lambda":
        params = [(name, None) for name in node[1]]
        fn = Function("<anonymous>", params, [("return", 0, node[2])])
        fn.defining_env = env          # closes over the scope it was made in
        return fn

    if tag == "in":
        return is_member(eval_expr(node[1], env), eval_expr(node[2], env))

    if tag == "notin":
        return not is_member(eval_expr(node[1], env), eval_expr(node[2], env))

    if tag == "listcomp":
        _, out_expr, var, iter_expr, cond = node
        loop_env = Environment(env)
        result = []
        for item in iterate(eval_expr(iter_expr, env)):
            loop_env.define(var, item)
            if cond is None or truthy(eval_expr(cond, loop_env)):
                result.append(eval_expr(out_expr, loop_env))
        return result

    if tag == "mapcomp":
        _, key_expr, val_expr, var, iter_expr, cond = node
        loop_env = Environment(env)
        result = {}
        for item in iterate(eval_expr(iter_expr, env)):
            loop_env.define(var, item)
            if cond is None or truthy(eval_expr(cond, loop_env)):
                result[eval_expr(key_expr, loop_env)] = eval_expr(val_expr, loop_env)
        return result

    if tag == "sliceop":
        target = eval_expr(node[1], env)
        if not isinstance(target, (list, str)):
            raise VantaError("you can only slice a list or text")
        start = eval_expr(node[2], env) if node[2] is not None else None
        end = eval_expr(node[3], env) if node[3] is not None else None
        for bound in (start, end):
            if bound is not None and (isinstance(bound, bool) or not isinstance(bound, int)):
                raise VantaError("slice positions must be whole numbers")
        return target[start:end]

    raise VantaError(f"internal error: unknown expression {tag}")


# --------------------------------------------------------------------------
# Calling: functions, builtins, methods, constructors
# --------------------------------------------------------------------------

def resolve_callee(target, env):
    if target[0] == "name":
        name = target[1]
        value = env.get(name)
        if value is not _MISSING:
            return value
        if name in BUILTIN_VALUES:
            return BUILTIN_VALUES[name]
        raise VantaError(f"I don't know a function called '{name}'")
    return eval_expr(target, env)


def apply_callable(callee, args):
    if isinstance(callee, Function):
        return call_function(callee, args)
    if isinstance(callee, Builtin):
        return callee.fn(args)
    if isinstance(callee, BoundMethod):
        return call_method(callee.inst, callee.fn, args)
    if isinstance(callee, VantaType):
        return construct_instance(callee, args)
    raise VantaError("that value is not a function you can call")


def bind_params(name, params, args, local):
    # Writes straight into local.vars and only builds the (quoted) error label
    # on the failure paths — this runs on every function/method call.
    nargs = len(args)
    if nargs > len(params):
        raise VantaError(f"'{name}' takes at most {len(params)} value(s), "
                         f"but got {nargs}")
    lv = local.vars
    for i, (pname, default) in enumerate(params):
        if i < nargs:
            lv[pname] = args[i]
        elif default is not None:
            lv[pname] = eval_expr(default, local)
        else:
            raise VantaError(f"'{name}' is missing a value for '{pname}'")


def call_function(fn, args):
    body = fn.compiled
    if body is None:
        body = fn.compiled = compile_block(fn.body)
    local = Environment(fn.defining_env or GLOBAL_ENV)
    bind_params(fn.name, fn.params, args, local)
    sig = body(local)
    if type(sig) is _Return:
        return sig.value
    return None


def call_method(inst, fn, args):
    body = fn.compiled
    if body is None:
        body = fn.compiled = compile_block(fn.body)
    local = Environment(GLOBAL_ENV)
    lv = local.vars
    lv["me"] = inst
    parent_type = fn.owner.parent if getattr(fn, "owner", None) else None
    lv["super"] = SuperProxy(inst, parent_type)
    bind_params(fn.name, fn.params, args, local)
    sig = body(local)
    if type(sig) is _Return:
        return sig.value
    return None


def construct_instance(vtype, args):
    inst = VantaInstance(vtype, {f: None for f in vtype.fields})
    if "setup" in vtype.methods:
        call_method(inst, vtype.methods["setup"], args)
    elif len(args) == len(vtype.fields):
        for field, value in zip(vtype.fields, args):
            inst.attrs[field] = value
    else:
        raise VantaError(f"{vtype.name} expects {len(vtype.fields)} value(s) "
                         f"({', '.join(vtype.fields)}), but got {len(args)}")
    return inst


def is_instance_of(value, vtype):
    if not isinstance(value, VantaInstance):
        return False
    t = value.vtype
    while t is not None:
        if t is vtype:
            return True
        t = t.parent
    return False


def get_attr(obj, attr):
    if isinstance(obj, SuperProxy):
        if obj.parent_type is None:
            raise VantaError("there is no parent type to reach with 'super'")
        method = obj.parent_type.methods.get(attr)
        if method is None:
            raise VantaError(f"the parent type has no '{attr}'")
        return BoundMethod(obj.inst, method)
    if isinstance(obj, VantaInstance):
        if attr in obj.attrs:
            return obj.attrs[attr]
        if attr in obj.vtype.methods:
            return BoundMethod(obj, obj.vtype.methods[attr])
        raise VantaError(f"a {obj.vtype.name} has no '{attr}'")
    raise VantaError("you can only use '.' on an object made from a type")


def set_attr(obj, attr, value):
    if isinstance(obj, VantaInstance):
        obj.attrs[attr] = value
        return
    raise VantaError("you can only set a '.' field on an object made from a type")


def eval_value_group(value_nodes, count, env):
    """Resolve the right-hand side of a multiple assignment into `count`
    values. One list value gets unpacked; several values are taken as-is."""
    if len(value_nodes) == 1:
        value = eval_expr(value_nodes[0], env)
        if not isinstance(value, list):
            raise VantaError("to unpack into several names, the value must be a list")
        if len(value) != count:
            raise VantaError(f"expected {count} values but the list has {len(value)}")
        return list(value)
    if len(value_nodes) != count:
        raise VantaError(f"there are {count} names but {len(value_nodes)} values")
    return [eval_expr(node, env) for node in value_nodes]


def do_assign(target, value, env):
    tag = target[0]
    if tag == "name":
        result = env.assign(target[1], value)
        if result == "const":
            raise VantaError(f"'{target[1]}' is fixed and can't be changed")
        if not result:
            raise VantaError(f"'{target[1]}' doesn't exist yet "
                             f"(use: let {target[1]} be ...)")
    elif tag == "index":
        set_index(eval_expr(target[1], env), eval_expr(target[2], env), value)
    elif tag == "getattr":
        set_attr(eval_expr(target[1], env), target[2], value)
    else:
        raise VantaError("you can't change that")


# --------------------------------------------------------------------------
# Operators and indexing
# --------------------------------------------------------------------------

def arithmetic(op, a, b):
    # Fast path: both operands are plain numbers (the overwhelmingly common
    # case). `type(x) is int` excludes bool — booleans aren't numbers here — so
    # this is exactly as strict as check_numbers, but without the isinstance
    # calls and the list/text special-casing below.
    ta = type(a)
    tb = type(b)
    if (ta is int or ta is float) and (tb is int or tb is float):
        if op == "+":
            return a + b
        if op == "-":
            return a - b
        if op == "*":
            return a * b
        if op == "%":
            if b == 0:
                raise VantaError("you can't take the remainder with zero")
            return a % b
        if op == "^":
            return a ** b
        if op == "/":
            if b == 0:
                raise VantaError("you can't divide by zero")
            result = a / b
            return int(result) if result == int(result) else result

    if op == "+":
        if isinstance(a, list) and isinstance(b, list):
            return a + b
        if isinstance(a, str) or isinstance(b, str):
            return display(a) + display(b)
        check_numbers(a, b, "+")
        return a + b
    if op == "*":
        # text/list repetition: "ab" * 3, [0] * 4
        if isinstance(a, (str, list)) and isinstance(b, int) and not isinstance(b, bool):
            return a * b
        if isinstance(b, (str, list)) and isinstance(a, int) and not isinstance(a, bool):
            return b * a
    check_numbers(a, b, op)
    if op == "-":
        return a - b
    if op == "*":
        return a * b
    if op == "^":
        return a ** b
    if op == "%":
        if b == 0:
            raise VantaError("you can't take the remainder with zero")
        return a % b
    if op == "/":
        if b == 0:
            raise VantaError("you can't divide by zero")
        result = a / b
        return int(result) if result == int(result) else result


def check_numbers(a, b, op):
    for value in (a, b):
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise VantaError(f"I can only do math ({op}) with numbers")


def compare(op, a, b):
    if op == "==":
        return a == b
    if op == "!=":
        return a != b
    ta = type(a)
    tb = type(b)
    both_numbers = (ta is int or ta is float) and (tb is int or tb is float)
    both_text = ta is str and tb is str
    if not (both_numbers or both_text):
        raise VantaError("I can only compare numbers with numbers, or text with text")
    if op == ">":
        return a > b
    if op == "<":
        return a < b
    if op == ">=":
        return a >= b
    if op == "<=":
        return a <= b


def get_index(collection, index):
    if isinstance(collection, (list, str)):
        return collection[whole_index(index, len(collection))]
    if isinstance(collection, dict):
        if index not in collection:
            raise VantaError(f"this map has no key {display_item(index)}")
        return collection[index]
    raise VantaError("you can only look inside a list, text, or map")


def set_index(collection, index, value):
    if isinstance(collection, list):
        collection[whole_index(index, len(collection))] = value
    elif isinstance(collection, dict):
        collection[index] = value
    else:
        raise VantaError("you can only change a position inside a list or map")


def whole_index(index, length):
    if isinstance(index, bool) or not isinstance(index, int):
        raise VantaError("a position must be a whole number")
    resolved = index + length if index < 0 else index   # -1 means the last item
    if resolved < 0 or resolved >= length:
        raise VantaError(f"position {index} is outside the range "
                         f"-{length}..{length - 1}")
    return resolved


def is_member(value, collection):
    if isinstance(collection, (list, str, dict)):
        return value in collection
    raise VantaError("'in' needs a list, text, or map on the right side")


def iterate_pairs(value):
    if isinstance(value, dict):
        return list(value.items())
    if isinstance(value, (list, str)):
        return list(enumerate(value))
    raise VantaError("looping with two names needs a map, list, or text")


def iterate(value):
    if isinstance(value, list):
        return list(value)
    if isinstance(value, str):
        return list(value)
    if isinstance(value, dict):
        return list(value.keys())
    raise VantaError("you can only loop over a list, text, or map")


# ===========================================================================
# STANDARD LIBRARY  (built-in functions)
# ===========================================================================

def _need(args, count, name):
    if len(args) != count:
        raise VantaError(f"{name} expects {count} value(s), got {len(args)}")


def int_arg(value, name):
    if isinstance(value, bool) or not isinstance(value, int):
        raise VantaError(f"{name} needs whole numbers")
    return value


def num_arg(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise VantaError(f"{name} needs a number")
    return value


def tidy_number(value):
    """Turn 4.0 into 4 but leave 4.5 alone."""
    if isinstance(value, float) and value == int(value):
        return int(value)
    return value


# ---- conversions & inspection -------------------------------------------

def b_length(args):
    _need(args, 1, "length")
    if isinstance(args[0], (str, list, dict)):
        return len(args[0])
    raise VantaError("length works on text, lists, or maps")


def b_text(args):
    _need(args, 1, "text")
    return display(args[0])


def b_number(args):
    _need(args, 1, "number")
    v = args[0]
    if isinstance(v, bool):
        raise VantaError("can't turn yes/no into a number")
    if isinstance(v, (int, float)):
        return v
    if isinstance(v, str):
        try:
            return int(v)
        except ValueError:
            try:
                return float(v)
            except ValueError:
                raise VantaError(f'"{v}" is not a number')
    raise VantaError("can't turn that into a number")


def b_type_of(args):
    _need(args, 1, "type_of")
    return type_name(args[0])


def b_is_number(args):
    _need(args, 1, "is_number")
    return isinstance(args[0], (int, float)) and not isinstance(args[0], bool)


def b_is_text(args):
    _need(args, 1, "is_text")
    return isinstance(args[0], str)


def b_is_list(args):
    _need(args, 1, "is_list")
    return isinstance(args[0], list)


def b_is_map(args):
    _need(args, 1, "is_map")
    return isinstance(args[0], dict)


def b_is_function(args):
    _need(args, 1, "is_function")
    return isinstance(args[0], (Function, Builtin, BoundMethod))


def b_is_nothing(args):
    _need(args, 1, "is_nothing")
    return args[0] is None


# ---- text helpers --------------------------------------------------------

def b_upper(args):
    _need(args, 1, "uppercase")
    return display(args[0]).upper()


def b_lower(args):
    _need(args, 1, "lowercase")
    return display(args[0]).lower()


def b_trim(args):
    _need(args, 1, "trim")
    return display(args[0]).strip()


def b_replace(args):
    _need(args, 3, "replace")
    return display(args[0]).replace(display(args[1]), display(args[2]))


def b_starts_with(args):
    _need(args, 2, "starts_with")
    return display(args[0]).startswith(display(args[1]))


def b_ends_with(args):
    _need(args, 2, "ends_with")
    return display(args[0]).endswith(display(args[1]))


def b_find(args):
    _need(args, 2, "find")
    return display(args[0]).find(display(args[1]))


def b_split(args):
    _need(args, 2, "split")
    return display(args[0]).split(display(args[1]))


def b_lines(args):
    _need(args, 1, "lines")
    return display(args[0]).split("\n")


def b_chr(args):
    _need(args, 1, "chr")
    n = int_arg(args[0], "chr")
    if n < 0 or n > 0x10FFFF:
        raise VantaError("chr needs a number from 0 to 1114111")
    return chr(n)


def b_code(args):
    _need(args, 1, "code")
    text = display(args[0])
    if len(text) == 0:
        raise VantaError("code needs a non-empty text value")
    return ord(text[0])


def b_pad_left(args):
    if len(args) not in (2, 3):
        raise VantaError("pad_left expects text, a width, and an optional character")
    fill = display(args[2])[:1] or " " if len(args) == 3 else " "
    return display(args[0]).rjust(int_arg(args[1], "pad_left"), fill)


def b_pad_right(args):
    if len(args) not in (2, 3):
        raise VantaError("pad_right expects text, a width, and an optional character")
    fill = display(args[2])[:1] or " " if len(args) == 3 else " "
    return display(args[0]).ljust(int_arg(args[1], "pad_right"), fill)


# ---- number helpers ------------------------------------------------------

def b_abs(args):
    _need(args, 1, "abs")
    return abs(num_arg(args[0], "abs"))


def b_round(args):
    _need(args, 1, "round")
    return round(num_arg(args[0], "round"))


def b_floor(args):
    _need(args, 1, "floor")
    return math.floor(num_arg(args[0], "floor"))


def b_ceil(args):
    _need(args, 1, "ceil")
    return math.ceil(num_arg(args[0], "ceil"))


def b_sqrt(args):
    _need(args, 1, "sqrt")
    value = num_arg(args[0], "sqrt")
    if value < 0:
        raise VantaError("can't take the square root of a negative number")
    return tidy_number(math.sqrt(value))


def b_power(args):
    _need(args, 2, "power")
    return num_arg(args[0], "power") ** num_arg(args[1], "power")


def b_sin(args):
    _need(args, 1, "sin")
    return math.sin(num_arg(args[0], "sin"))


def b_cos(args):
    _need(args, 1, "cos")
    return math.cos(num_arg(args[0], "cos"))


def b_tan(args):
    _need(args, 1, "tan")
    return math.tan(num_arg(args[0], "tan"))


def b_log(args):
    if len(args) == 1:
        value = num_arg(args[0], "log")
        if value <= 0:
            raise VantaError("log needs a number above zero")
        return math.log(value)
    if len(args) == 2:
        value, base = num_arg(args[0], "log"), num_arg(args[1], "log")
        if value <= 0 or base <= 0:
            raise VantaError("log needs numbers above zero")
        return math.log(value, base)
    raise VantaError("log expects a number (and an optional base)")


def b_exp(args):
    _need(args, 1, "exp")
    return math.exp(num_arg(args[0], "exp"))


def b_sum(args):
    _need(args, 1, "sum")
    if not isinstance(args[0], list):
        raise VantaError("sum needs a list of numbers")
    total = 0
    for value in args[0]:
        total = total + num_arg(value, "sum")
    return total


def b_product(args):
    _need(args, 1, "product")
    if not isinstance(args[0], list):
        raise VantaError("product needs a list of numbers")
    result = 1
    for value in args[0]:
        result = result * num_arg(value, "product")
    return result


def _less(a, b):
    both_num = (isinstance(a, (int, float)) and isinstance(b, (int, float))
                and not isinstance(a, bool) and not isinstance(b, bool))
    both_text = isinstance(a, str) and isinstance(b, str)
    if not (both_num or both_text):
        raise VantaError("min/max need all numbers or all text")
    return a < b


def _collect(args, name):
    values = args[0] if len(args) == 1 and isinstance(args[0], list) else list(args)
    if not values:
        raise VantaError(f"{name} needs at least one value")
    return values


def b_min(args):
    values = _collect(args, "min")
    best = values[0]
    for v in values[1:]:
        if _less(v, best):
            best = v
    return best


def b_max(args):
    values = _collect(args, "max")
    best = values[0]
    for v in values[1:]:
        if _less(best, v):
            best = v
    return best


def b_random(args):
    _need(args, 2, "random")
    low, high = int_arg(args[0], "random"), int_arg(args[1], "random")
    if low > high:
        raise VantaError("random needs the low number first")
    return random.randint(low, high)


def b_now(args):
    _need(args, 0, "now")
    return int(time.time())


# ---- list & map helpers --------------------------------------------------

def b_first(args):
    _need(args, 1, "first")
    if isinstance(args[0], (list, str)) and len(args[0]) > 0:
        return args[0][0]
    raise VantaError("first needs a non-empty list or text")


def b_last(args):
    _need(args, 1, "last")
    if isinstance(args[0], (list, str)) and len(args[0]) > 0:
        return args[0][-1]
    raise VantaError("last needs a non-empty list or text")


def b_range(args):
    if len(args) == 1:
        return list(range(int_arg(args[0], "range")))
    if len(args) == 2:
        return list(range(int_arg(args[0], "range"), int_arg(args[1], "range")))
    raise VantaError("range expects 1 or 2 numbers")


def b_contains(args):
    _need(args, 2, "contains")
    coll, item = args
    if isinstance(coll, (list, str, dict)):
        return item in coll
    raise VantaError("contains works on lists, text, or maps")


def b_join(args):
    _need(args, 2, "join")
    seq, sep = args
    if not isinstance(seq, list):
        raise VantaError("join needs a list as its first value")
    return display(sep).join(display(x) for x in seq)


def b_keys(args):
    _need(args, 1, "keys")
    if not isinstance(args[0], dict):
        raise VantaError("keys needs a map")
    return list(args[0].keys())


def b_values(args):
    _need(args, 1, "values")
    if not isinstance(args[0], dict):
        raise VantaError("values needs a map")
    return list(args[0].values())


def b_sort(args):
    _need(args, 1, "sort")
    if not isinstance(args[0], list):
        raise VantaError("sort needs a list")
    try:
        return sorted(args[0])
    except TypeError:
        raise VantaError("sort needs a list of all numbers or all text")


def b_reverse(args):
    _need(args, 1, "reverse")
    if isinstance(args[0], list):
        return list(reversed(args[0]))
    if isinstance(args[0], str):
        return args[0][::-1]
    raise VantaError("reverse needs a list or text")


def b_slice(args):
    _need(args, 3, "slice")
    seq = args[0]
    if not isinstance(seq, (list, str)):
        raise VantaError("slice needs a list or text")
    return seq[int_arg(args[1], "slice"):int_arg(args[2], "slice")]


def b_push(args):
    _need(args, 2, "push")
    if not isinstance(args[0], list):
        raise VantaError("push needs a list")
    args[0].append(args[1])
    return None


def b_pop(args):
    _need(args, 1, "pop")
    if not isinstance(args[0], list) or not args[0]:
        raise VantaError("pop needs a non-empty list")
    return args[0].pop()


def b_remove_at(args):
    _need(args, 2, "remove_at")
    if not isinstance(args[0], list):
        raise VantaError("remove_at needs a list")
    return args[0].pop(whole_index(args[1], len(args[0])))


# ---- higher-order functions ---------------------------------------------

def b_map(args):
    _need(args, 2, "map")
    fn, seq = args
    if not isinstance(seq, list):
        raise VantaError("map needs a function and a list")
    return [apply_callable(fn, [x]) for x in seq]


def b_keep(args):
    _need(args, 2, "keep")
    fn, seq = args
    if not isinstance(seq, list):
        raise VantaError("keep needs a function and a list")
    return [x for x in seq if truthy(apply_callable(fn, [x]))]


def b_reduce(args):
    _need(args, 3, "reduce")
    fn, seq, start = args
    if not isinstance(seq, list):
        raise VantaError("reduce needs a function, a list, and a starting value")
    acc = start
    for x in seq:
        acc = apply_callable(fn, [acc, x])
    return acc


def b_each(args):
    _need(args, 2, "each")
    fn, seq = args
    if not isinstance(seq, list):
        raise VantaError("each needs a function and a list")
    for x in seq:
        apply_callable(fn, [x])
    return None


def b_count_where(args):
    _need(args, 2, "count_where")
    fn, seq = args
    if not isinstance(seq, list):
        raise VantaError("count_where needs a function and a list")
    return sum(1 for x in seq if truthy(apply_callable(fn, [x])))


def b_find_where(args):
    _need(args, 2, "find_where")
    fn, seq = args
    if not isinstance(seq, list):
        raise VantaError("find_where needs a function and a list")
    for x in seq:
        if truthy(apply_callable(fn, [x])):
            return x
    return None


def b_sort_by(args):
    _need(args, 2, "sort_by")
    fn, seq = args
    if not isinstance(seq, list):
        raise VantaError("sort_by needs a function and a list")
    try:
        return sorted(seq, key=lambda x: apply_callable(fn, [x]))
    except TypeError:
        raise VantaError("sort_by's keys must be all numbers or all text")


# ---- errors --------------------------------------------------------------

def b_fail(args):
    _need(args, 1, "fail")
    raise VantaError(display(args[0]))


def b_assert(args):
    if len(args) not in (1, 2):
        raise VantaError("assert expects a condition and an optional message")
    if not truthy(args[0]):
        raise VantaError(display(args[1]) if len(args) == 2 else "assertion failed")
    return None


# ---- files, system, json -------------------------------------------------

def b_read_file(args):
    _need(args, 1, "read_file")
    try:
        with open(display(args[0]), "r") as f:
            return f.read()
    except OSError as e:
        raise VantaError(f"could not read file: {e}")


def b_write_file(args):
    _need(args, 2, "write_file")
    try:
        with open(display(args[0]), "w") as f:
            f.write(display(args[1]))
    except OSError as e:
        raise VantaError(f"could not write file: {e}")
    return None


def b_run(args):
    if len(args) not in (1, 2):
        raise VantaError("run expects a command (and an optional directory)")
    cwd = display(args[1]) if len(args) == 2 else None
    result = subprocess.run(display(args[0]), shell=True,
                            capture_output=True, text=True, cwd=cwd)
    return (result.stdout + result.stderr).rstrip("\n")


def b_shell(args):
    if len(args) not in (1, 2, 3):
        raise VantaError("shell expects a command, an optional directory, "
                         "and an optional map of environment variables")
    cwd = display(args[1]) if len(args) >= 2 else None
    sub_env = None
    if len(args) == 3:
        if not isinstance(args[2], dict):
            raise VantaError("shell's third value must be a map of variables")
        sub_env = dict(os.environ)
        for key, value in args[2].items():
            sub_env[display(key)] = display(value)
    result = subprocess.run(display(args[0]), shell=True, capture_output=True,
                            text=True, cwd=cwd, env=sub_env)
    return {"output": (result.stdout + result.stderr).rstrip("\n"),
            "code": result.returncode}


def b_arguments(args):
    _need(args, 0, "arguments")
    return list(PROGRAM_ARGS)


def b_env(args):
    _need(args, 1, "env")
    return os.environ.get(display(args[0]), "")


def b_make_dir(args):
    _need(args, 1, "make_dir")
    os.makedirs(display(args[0]), exist_ok=True)
    return None


def b_remove_path(args):
    _need(args, 1, "remove_path")
    path = display(args[0])
    if os.path.isdir(path):
        shutil.rmtree(path)
    elif os.path.exists(path):
        os.remove(path)
    return None


def b_list_dir(args):
    _need(args, 1, "list_dir")
    try:
        return sorted(os.listdir(display(args[0])))
    except OSError as e:
        raise VantaError(f"could not list directory: {e}")


def b_path_exists(args):
    _need(args, 1, "path_exists")
    return os.path.exists(display(args[0]))


def b_copy_path(args):
    _need(args, 2, "copy_path")
    src, dst = display(args[0]), display(args[1])
    try:
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            shutil.copy2(src, dst)
    except OSError as e:
        raise VantaError(f"could not copy {src}: {e}")
    return None


def b_to_json(args):
    _need(args, 1, "to_json")
    try:
        return json.dumps(args[0])
    except TypeError:
        raise VantaError("that value can't be turned into JSON")


def b_from_json(args):
    _need(args, 1, "from_json")
    try:
        return json.loads(display(args[0]))
    except ValueError:
        raise VantaError("that text is not valid JSON")


def b_interpreter(args):
    _need(args, 0, "interpreter")
    return os.path.abspath(sys.argv[0])


# ---- bytes and bitwise (needed for emulators and low-level work) ---------

def b_read_bytes(args):
    _need(args, 1, "read_bytes")
    try:
        with open(display(args[0]), "rb") as f:
            return list(f.read())          # a list of whole numbers 0..255
    except OSError as e:
        raise VantaError(f"could not read file: {e}")


def b_band(args):
    _need(args, 2, "band")
    return int_arg(args[0], "band") & int_arg(args[1], "band")


def b_bor(args):
    _need(args, 2, "bor")
    return int_arg(args[0], "bor") | int_arg(args[1], "bor")


def b_bxor(args):
    _need(args, 2, "bxor")
    return int_arg(args[0], "bxor") ^ int_arg(args[1], "bxor")


def b_bnot(args):
    _need(args, 2, "bnot")
    value, bits = int_arg(args[0], "bnot"), int_arg(args[1], "bnot")
    return (~value) & ((1 << bits) - 1)


def b_shift_left(args):
    _need(args, 2, "shift_left")
    return int_arg(args[0], "shift_left") << int_arg(args[1], "shift_left")


def b_shift_right(args):
    _need(args, 2, "shift_right")
    return int_arg(args[0], "shift_right") >> int_arg(args[1], "shift_right")


# ---- type checks ---------------------------------------------------------

def b_is_a(args):
    _need(args, 2, "is_a")
    if not isinstance(args[1], VantaType):
        raise VantaError("is_a's second value must be a type")
    return is_instance_of(args[0], args[1])


# ---- regular expressions -------------------------------------------------

def b_matches(args):
    _need(args, 2, "matches")
    try:
        return re.search(display(args[1]), display(args[0])) is not None
    except re.error as e:
        raise VantaError(f"that pattern is not valid: {e}")


def b_find_all(args):
    _need(args, 2, "find_all")
    try:
        return [m.group(0) for m in re.finditer(display(args[1]), display(args[0]))]
    except re.error as e:
        raise VantaError(f"that pattern is not valid: {e}")


def b_replace_all(args):
    _need(args, 3, "replace_all")
    try:
        return re.sub(display(args[1]), display(args[2]), display(args[0]))
    except re.error as e:
        raise VantaError(f"that pattern is not valid: {e}")


# ---- dates & time --------------------------------------------------------

def b_today(args):
    _need(args, 0, "today")
    return time.strftime("%Y-%m-%d")


def b_clock(args):
    _need(args, 0, "clock")
    return time.strftime("%H:%M:%S")


# ---- the web: HTTP client, HTTP server, and friends ----------------------
# These make Vanta a real full-stack language: talk to APIs, and serve web
# pages / JSON like Node, Flask, or a Go/Rust server would. Imports are lazy
# so loading vanta.py never needs a network stack (e.g. in the browser).

def _headers_map(value, name):
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise VantaError(f"{name}'s headers must be a map")
    return {display(k): display(v) for k, v in value.items()}


def _http_request(method, url, body, headers):
    import urllib.request
    import urllib.error
    hdrs = _headers_map(headers, "http")
    data = None
    if body is not None and body != "":
        if isinstance(body, (dict, list)):
            data = json.dumps(body).encode("utf-8")
            hdrs.setdefault("Content-Type", "application/json")
        else:
            data = display(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            text = resp.read().decode("utf-8", "replace")
            return {"status": resp.status, "body": text,
                    "headers": {k: v for k, v in resp.headers.items()}}
    except urllib.error.HTTPError as e:
        text = e.read().decode("utf-8", "replace")
        return {"status": e.code, "body": text,
                "headers": {k: v for k, v in (e.headers or {}).items()}}
    except urllib.error.URLError as e:
        raise VantaError(f"could not reach {url}: {getattr(e, 'reason', e)}")


def b_http_get(args):
    if len(args) not in (1, 2):
        raise VantaError("http_get expects a url (and an optional headers map)")
    return _http_request("GET", display(args[0]), None,
                         args[1] if len(args) == 2 else None)


def b_http_post(args):
    if len(args) not in (2, 3):
        raise VantaError("http_post expects a url and a body "
                         "(and an optional headers map)")
    return _http_request("POST", display(args[0]), args[1],
                         args[2] if len(args) == 3 else None)


def b_http_request(args):
    if len(args) not in (2, 3, 4):
        raise VantaError("http_request expects a method, a url, an optional "
                         "body, and an optional headers map")
    return _http_request(display(args[0]).upper(), display(args[1]),
                         args[2] if len(args) >= 3 else None,
                         args[3] if len(args) == 4 else None)


def b_serve(args):
    if len(args) not in (1, 2):
        raise VantaError("serve expects a port and a handler function "
                         "(or just a handler — port defaults to 8080)")
    if len(args) == 1:
        port, handler = 8080, args[0]
    else:
        port, handler = int_arg(args[0], "serve"), args[1]
    if not isinstance(handler, (Function, Builtin, BoundMethod)):
        raise VantaError("serve's handler must be a function that takes a "
                         "request map and gives back a page (text or a map)")

    import urllib.parse
    import threading
    from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

    # Slow clients get their own thread (so one can't block the whole server),
    # but the Vanta handler itself runs under a lock — the interpreter shares
    # one global scope, so handlers run one at a time and never race.
    handler_lock = threading.Lock()

    class VantaHandler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _reply(self, status, text, ctype, extra):
            blob = text.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(blob)))
            for k, v in extra.items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(blob)

        def _handle(self, method):
            parsed = urllib.parse.urlparse(self.path)
            query = {k: (v[0] if len(v) == 1 else v)
                     for k, v in urllib.parse.parse_qs(parsed.query).items()}
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
            request = {"method": method, "path": parsed.path, "query": query,
                       "headers": {k: v for k, v in self.headers.items()},
                       "body": body}
            try:
                with handler_lock:
                    result = apply_callable(handler, [request])
            except VantaError as e:
                self._reply(500, f"Vanta error: {e}", "text/plain; charset=utf-8", {})
                return
            status, ctype, extra = 200, "text/html; charset=utf-8", {}
            if isinstance(result, dict):
                status = int(result.get("status", 200))
                raw = result.get("body", "")
                if isinstance(raw, (dict, list)):
                    text = json.dumps(raw)
                    ctype = "application/json"
                else:
                    text = display(raw)
                if "type" in result or "content_type" in result:
                    ctype = display(result.get("type", result.get("content_type")))
                hh = result.get("headers")
                if isinstance(hh, dict):
                    extra = {display(k): display(v) for k, v in hh.items()}
            else:
                text = display(result)
            self._reply(status, text, ctype, extra)

        def do_GET(self):     self._handle("GET")
        def do_POST(self):    self._handle("POST")
        def do_PUT(self):     self._handle("PUT")
        def do_DELETE(self):  self._handle("DELETE")
        def do_PATCH(self):   self._handle("PATCH")
        def do_HEAD(self):    self._handle("HEAD")

    server = ThreadingHTTPServer(("", port), VantaHandler)
    server.daemon_threads = True
    print(f"Vanta server running on http://localhost:{port}  "
          f"(press Stop / Ctrl-C to quit)")
    sys.stdout.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        print("Server stopped.")
    return None


def b_sleep(args):
    _need(args, 1, "sleep")
    secs = args[0]
    if isinstance(secs, bool) or not isinstance(secs, (int, float)):
        raise VantaError("sleep expects a number of seconds")
    time.sleep(secs)
    return None


def b_url_encode(args):
    _need(args, 1, "url_encode")
    import urllib.parse
    return urllib.parse.quote(display(args[0]), safe="")


def b_url_decode(args):
    _need(args, 1, "url_decode")
    import urllib.parse
    return urllib.parse.unquote(display(args[0]))


def b_html_escape(args):
    _need(args, 1, "html_escape")
    s = display(args[0])
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace('"', "&quot;").replace("'", "&#39;"))


BUILTINS = {
    # conversions & inspection
    "length": b_length, "text": b_text, "number": b_number,
    "type_of": b_type_of, "is_number": b_is_number, "is_text": b_is_text,
    "is_list": b_is_list, "is_map": b_is_map, "is_function": b_is_function,
    "is_nothing": b_is_nothing, "is_a": b_is_a,
    # regular expressions & dates
    "matches": b_matches, "find_all": b_find_all, "replace_all": b_replace_all,
    "today": b_today, "clock": b_clock,
    # text
    "uppercase": b_upper, "lowercase": b_lower, "trim": b_trim,
    "replace": b_replace, "starts_with": b_starts_with, "ends_with": b_ends_with,
    "find": b_find, "split": b_split, "lines": b_lines,
    "pad_left": b_pad_left, "pad_right": b_pad_right,
    "chr": b_chr, "code": b_code,
    # numbers
    "abs": b_abs, "round": b_round, "floor": b_floor, "ceil": b_ceil,
    "sqrt": b_sqrt, "power": b_power, "min": b_min, "max": b_max,
    "sin": b_sin, "cos": b_cos, "tan": b_tan, "log": b_log, "exp": b_exp,
    "sum": b_sum, "product": b_product,
    "random": b_random, "now": b_now,
    # lists & maps
    "first": b_first, "last": b_last, "range": b_range, "contains": b_contains,
    "join": b_join, "keys": b_keys, "values": b_values, "sort": b_sort,
    "reverse": b_reverse, "slice": b_slice, "push": b_push, "pop": b_pop,
    "remove_at": b_remove_at,
    # higher-order
    "map": b_map, "keep": b_keep, "reduce": b_reduce, "each": b_each,
    "count_where": b_count_where, "find_where": b_find_where, "sort_by": b_sort_by,
    # errors
    "fail": b_fail, "assert": b_assert,
    # files, system, json
    "read_file": b_read_file, "write_file": b_write_file, "run": b_run,
    "shell": b_shell, "arguments": b_arguments, "env": b_env,
    "make_dir": b_make_dir, "remove_path": b_remove_path, "list_dir": b_list_dir,
    "path_exists": b_path_exists, "copy_path": b_copy_path,
    "to_json": b_to_json, "from_json": b_from_json, "interpreter": b_interpreter,
    # bytes & bitwise
    "read_bytes": b_read_bytes, "band": b_band, "bor": b_bor, "bxor": b_bxor,
    "bnot": b_bnot, "shift_left": b_shift_left, "shift_right": b_shift_right,
    # the web: HTTP client, HTTP server, and helpers
    "http_get": b_http_get, "http_post": b_http_post,
    "http_request": b_http_request, "serve": b_serve, "sleep": b_sleep,
    "url_encode": b_url_encode, "url_decode": b_url_decode,
    "html_escape": b_html_escape,
}

# A first-class value for every builtin, so they can be passed to map/keep/etc.
BUILTIN_VALUES = {name: Builtin(name, fn) for name, fn in BUILTINS.items()}


# ===========================================================================
# SHARED HELPERS
# ===========================================================================

def type_name(value):
    if value is None:
        return "nothing"
    if isinstance(value, bool):
        return "yes-no"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "text"
    if isinstance(value, list):
        return "list"
    if isinstance(value, dict):
        return "map"
    if isinstance(value, (Function, Builtin, BoundMethod)):
        return "function"
    if isinstance(value, VantaType):
        return "type"
    if isinstance(value, VantaInstance):
        return value.vtype.name
    return "value"


def truthy(value):
    if value is None:
        return False
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, (str, list, dict)):
        return len(value) > 0
    return True


def display(value):
    if value is None:
        return "nothing"
    if value is True:
        return "yes"
    if value is False:
        return "no"
    if isinstance(value, float) and value == int(value):
        return str(int(value))
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "[" + ", ".join(display_item(x) for x in value) + "]"
    if isinstance(value, dict):
        return "{" + ", ".join(display_item(k) + ": " + display_item(v)
                               for k, v in value.items()) + "}"
    if isinstance(value, Function):
        return f"<function {value.name}>"
    if isinstance(value, Builtin):
        return f"<builtin {value.name}>"
    if isinstance(value, BoundMethod):
        return f"<method {value.fn.name}>"
    if isinstance(value, VantaType):
        return f"<type {value.name}>"
    if isinstance(value, VantaInstance):
        vt = value.vtype
        method = vt.methods.get("show")
        if method is not None and len(method.params) == 0:
            return display(call_method(value, method, []))
        inner = ", ".join(f"{f}=" + display_item(value.attrs.get(f)) for f in vt.fields)
        return f"{vt.name}({inner})"
    return str(value)


def display_item(value):
    if isinstance(value, str):
        return '"' + value + '"'
    return display(value)


def smart_value(text):
    try:
        return int(text)
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text


RESERVED = {"if", "end", "otherwise", "repeat", "while", "for", "each", "in",
            "to", "give", "back", "say", "let", "change", "ask", "stop",
            "skip", "import", "is", "be", "and", "or", "not", "yes", "no",
            "nothing", "times", "at", "type", "has", "attempt", "rescue", "new",
            "from", "super", "match", "when", "increase", "decrease", "by",
            "make", "fix"}


def check_name(name, lineno):
    if not name or not (name[0].isalpha() or name[0] == "_"):
        raise VantaError(f"line {lineno}: '{name}' is not a valid name")
    if not all(c.isalnum() or c == "_" for c in name):
        raise VantaError(f"line {lineno}: '{name}' is not a valid name")
    if name in RESERVED:
        raise VantaError(f"line {lineno}: '{name}' is a reserved word, "
                         f"pick another name")


def load_lines(source):
    """Keep real line numbers; drop blank lines and # / note comments."""
    lines = []
    for i, raw in enumerate(source.splitlines(), start=1):
        text = raw.strip()
        if not text or text.startswith("#") or first_word(text) == "note":
            continue
        lines.append((i, text))
    return lines


IMPORTED = set()


def resolve_import(name):
    """Find a module by name. Search order: the path as given (so existing
    relative imports keep working), then the bundled standard library in
    lib/, then installed packages in ~/.vanta/packages/."""
    with_ext = name if name.endswith(".va") else name + ".va"
    here = os.path.dirname(os.path.abspath(__file__))
    packages = os.path.join(os.path.expanduser("~"), ".vanta", "packages")
    candidates = [
        name,
        with_ext,
        os.path.join(here, "lib", with_ext),
        os.path.join(packages, with_ext),
        os.path.join(packages, name, "main.va"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    raise VantaError(f"could not find a module named '{name}'")


def import_file(name):
    path = os.path.abspath(resolve_import(name))
    if path in IMPORTED:          # each module loads at most once
        return
    IMPORTED.add(path)
    try:
        with open(path, "r") as f:
            source = f.read()
    except OSError as e:
        raise VantaError(f"could not import {name}: {e}")
    compile_block(parse_program(load_lines(source)))(GLOBAL_ENV)


# ===========================================================================
# COMPILER  (turn the AST into nested Python closures, once, so we never
# re-walk the tree at run time)
#
# The tree-walker above re-decides what every node means on every visit — that
# dispatch is the dominant cost. Here we compile each node ONCE into a small
# Python function `f(env)` that does exactly its job: the operator, the variable,
# the call are all resolved at compile time. The closures reuse every semantic
# helper above (arithmetic, compare, get_index, apply_callable, ...), so the
# behaviour is identical to the tree-walker — only the dispatch disappears.
# A Function caches its compiled body (see call_function); the tree-walker stays
# as the engine for the few rare statements we don't specialise (e.g. 'type').
# ===========================================================================

# Control flow inside compiled code travels as a returned *signal* instead of a
# raised exception (raising on every function return / loop break is slow). A
# compiled statement returns None to mean "carry on", a _Return to unwind a
# function, or the _BREAK / _CONTINUE singletons inside a loop. Blocks and loops
# below check for these and propagate them. (The tree-walker still uses the
# ReturnSignal/BreakLoop/ContinueLoop exceptions, but it's now the cold path.)
class _Return:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value


_BREAK = object()
_CONTINUE = object()


def lookup_name(env, name):
    e = env
    while e is not None:
        v = e.vars
        if name in v:
            return v[name]
        e = e.parent
    b = BUILTIN_VALUES.get(name, _MISSING)
    if b is not _MISSING:
        return b
    raise VantaError(f"I don't know what '{name}' is yet")


def compile_expr(node):
    tag = node[0]

    if tag == "lit":
        value = node[1]
        return lambda env: value

    if tag == "name":
        name = node[1]
        # the env-chain walk is inlined here (no helper call) — name lookup is
        # the single most frequent operation in almost any program
        def ev(env):
            e = env
            while e is not None:
                v = e.vars
                if name in v:
                    return v[name]
                e = e.parent
            b = BUILTIN_VALUES.get(name, _MISSING)
            if b is not _MISSING:
                return b
            raise VantaError(f"I don't know what '{name}' is yet")
        return ev

    if tag == "arith":
        op = node[1]
        a = compile_expr(node[2])
        b = compile_expr(node[3])
        if op == "+":
            def ev(env):
                x = a(env); y = b(env)
                if type(x) is int and type(y) is int:
                    return x + y
                return arithmetic("+", x, y)
            return ev
        if op == "-":
            def ev(env):
                x = a(env); y = b(env)
                if type(x) is int and type(y) is int:
                    return x - y
                return arithmetic("-", x, y)
            return ev
        if op == "*":
            def ev(env):
                x = a(env); y = b(env)
                if type(x) is int and type(y) is int:
                    return x * y
                return arithmetic("*", x, y)
            return ev
        return lambda env: arithmetic(op, a(env), b(env))

    if tag == "cmp":
        op = node[1]
        a = compile_expr(node[2])
        b = compile_expr(node[3])
        if op == "==":
            return lambda env: a(env) == b(env)
        if op == "!=":
            return lambda env: a(env) != b(env)
        if op == "<":
            def ev(env):
                x = a(env); y = b(env)
                tx = type(x); ty = type(y)
                if (tx is int or tx is float) and (ty is int or ty is float):
                    return x < y
                return compare("<", x, y)
            return ev
        if op == ">":
            def ev(env):
                x = a(env); y = b(env)
                tx = type(x); ty = type(y)
                if (tx is int or tx is float) and (ty is int or ty is float):
                    return x > y
                return compare(">", x, y)
            return ev
        if op == "<=":
            def ev(env):
                x = a(env); y = b(env)
                tx = type(x); ty = type(y)
                if (tx is int or tx is float) and (ty is int or ty is float):
                    return x <= y
                return compare("<=", x, y)
            return ev
        if op == ">=":
            def ev(env):
                x = a(env); y = b(env)
                tx = type(x); ty = type(y)
                if (tx is int or tx is float) and (ty is int or ty is float):
                    return x >= y
                return compare(">=", x, y)
            return ev
        return lambda env: compare(op, a(env), b(env))

    if tag == "call":
        callee_node = node[1]
        cargs = [compile_expr(a) for a in node[2]]
        if callee_node[0] == "name":
            name = callee_node[1]
            def ev(env):
                e = env
                while e is not None:
                    v = e.vars
                    if name in v:
                        return apply_callable(v[name], [a(env) for a in cargs])
                    e = e.parent
                callee = BUILTIN_VALUES.get(name, _MISSING)
                if callee is _MISSING:
                    raise VantaError(f"I don't know a function called '{name}'")
                return apply_callable(callee, [a(env) for a in cargs])
            return ev
        cf = compile_expr(callee_node)
        return lambda env: apply_callable(cf(env), [a(env) for a in cargs])

    if tag == "index":
        a = compile_expr(node[1])
        b = compile_expr(node[2])
        return lambda env: get_index(a(env), b(env))

    if tag == "getattr":
        a = compile_expr(node[1])
        attr = node[2]
        return lambda env: get_attr(a(env), attr)

    if tag == "format":
        parts = [compile_expr(p) for p in node[1]]
        return lambda env: "".join(display(p(env)) for p in parts)

    if tag == "list":
        items = [compile_expr(i) for i in node[1]]
        return lambda env: [i(env) for i in items]

    if tag == "map":
        pairs = [(compile_expr(k), compile_expr(v)) for k, v in node[1]]
        def ev(env):
            result = {}
            for k, v in pairs:
                result[k(env)] = v(env)
            return result
        return ev

    if tag == "and":
        a = compile_expr(node[1]); b = compile_expr(node[2])
        def ev(env):
            if not truthy(a(env)):
                return False
            return truthy(b(env))
        return ev

    if tag == "or":
        a = compile_expr(node[1]); b = compile_expr(node[2])
        def ev(env):
            if truthy(a(env)):
                return True
            return truthy(b(env))
        return ev

    if tag == "not":
        a = compile_expr(node[1])
        return lambda env: not truthy(a(env))

    if tag == "neg":
        a = compile_expr(node[1])
        def ev(env):
            value = a(env)
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise VantaError("only numbers can be negative")
            return -value
        return ev

    if tag == "ternary":
        c = compile_expr(node[1]); t = compile_expr(node[2]); f = compile_expr(node[3])
        return lambda env: t(env) if truthy(c(env)) else f(env)

    if tag == "in":
        a = compile_expr(node[1]); b = compile_expr(node[2])
        return lambda env: is_member(a(env), b(env))

    if tag == "notin":
        a = compile_expr(node[1]); b = compile_expr(node[2])
        return lambda env: not is_member(a(env), b(env))

    if tag == "isa":
        val = compile_expr(node[1]); target = compile_expr(node[2])
        def ev(env):
            t = target(env)
            if not isinstance(t, VantaType):
                raise VantaError("the right side of 'is a' must be a type")
            return is_instance_of(val(env), t)
        return ev

    if tag == "lambda":
        params = [(name, None) for name in node[1]]
        body_ast = [("return", 0, node[2])]
        compiled_body = compile_block(body_ast)
        def ev(env):
            fn = Function("<anonymous>", params, body_ast)
            fn.compiled = compiled_body
            fn.defining_env = env
            return fn
        return ev

    if tag == "listcomp":
        _, out_expr, var, iter_expr, cond = node
        out = compile_expr(out_expr)
        itr = compile_expr(iter_expr)
        cnd = compile_expr(cond) if cond is not None else None
        def ev(env):
            loop_env = Environment(env)
            lv = loop_env.vars
            result = []
            for item in iterate(itr(env)):
                lv[var] = item
                if cnd is None or truthy(cnd(loop_env)):
                    result.append(out(loop_env))
            return result
        return ev

    if tag == "mapcomp":
        _, key_expr, val_expr, var, iter_expr, cond = node
        kc = compile_expr(key_expr)
        vc = compile_expr(val_expr)
        itr = compile_expr(iter_expr)
        cnd = compile_expr(cond) if cond is not None else None
        def ev(env):
            loop_env = Environment(env)
            lv = loop_env.vars
            result = {}
            for item in iterate(itr(env)):
                lv[var] = item
                if cnd is None or truthy(cnd(loop_env)):
                    result[kc(loop_env)] = vc(loop_env)
            return result
        return ev

    if tag == "sliceop":
        target = compile_expr(node[1])
        s = compile_expr(node[2]) if node[2] is not None else None
        e = compile_expr(node[3]) if node[3] is not None else None
        def ev(env):
            t = target(env)
            if not isinstance(t, (list, str)):
                raise VantaError("you can only slice a list or text")
            start = s(env) if s is not None else None
            end = e(env) if e is not None else None
            for bound in (start, end):
                if bound is not None and (isinstance(bound, bool) or not isinstance(bound, int)):
                    raise VantaError("slice positions must be whole numbers")
            return t[start:end]
        return ev

    # anything we didn't specialise: fall back to the tree-walker (correct, rare)
    return lambda env: eval_expr(node, env)


def compile_stmt(stmt):
    tag = stmt[0]

    if tag == "expr":
        n = compile_expr(stmt[2])
        def ex(env):
            n(env)
        return ex

    if tag == "let":
        name = stmt[2]; v = compile_expr(stmt[3])
        def ex(env):
            env.vars[name] = v(env)
        return ex

    if tag == "assign":
        target = stmt[2]; v = compile_expr(stmt[3])
        ttag = target[0]
        if ttag == "name":
            name = target[1]
            def ex(env):
                result = env.assign(name, v(env))
                if result == "const":
                    raise VantaError(f"'{name}' is fixed and can't be changed")
                if not result:
                    raise VantaError(f"'{name}' doesn't exist yet "
                                     f"(use: let {name} be ...)")
            return ex
        if ttag == "index":
            coll = compile_expr(target[1]); idx = compile_expr(target[2])
            def ex(env):
                set_index(coll(env), idx(env), v(env))
            return ex
        if ttag == "getattr":
            obj = compile_expr(target[1]); attr = target[2]
            def ex(env):
                set_attr(obj(env), attr, v(env))
            return ex
        return lambda env: do_assign(target, v(env), env)

    if tag == "if":
        branches = [(compile_expr(c), compile_block(body)) for c, body in stmt[2]]
        else_body = compile_block(stmt[3])
        def ex(env):
            for cond, body in branches:
                if truthy(cond(env)):
                    return body(env)
            return else_body(env)
        return ex

    if tag == "return":
        v = compile_expr(stmt[2])
        def ex(env):
            return _Return(v(env))
        return ex

    if tag == "say":
        node = stmt[2]
        if node is None:
            return lambda env: print("")
        n = compile_expr(node)
        return lambda env: print(display(n(env)))

    if tag == "mutate":
        _, _, target_node, sign, amount_node = stmt
        tget = compile_expr(target_node)
        amt = compile_expr(amount_node)
        def ex(env):
            new_value = arithmetic(sign, tget(env), amt(env))
            do_assign(target_node, new_value, env)
        return ex

    if tag == "while":
        cond = compile_expr(stmt[2]); body = compile_block(stmt[3])
        def ex(env):
            while truthy(cond(env)):
                sig = body(env)
                if sig is not None:
                    if sig is _CONTINUE:
                        continue
                    if sig is _BREAK:
                        break
                    return sig            # a _Return — unwind the function
            return None
        return ex

    if tag == "foreach":
        _, _, name, iter_node, body_ast = stmt
        itr = compile_expr(iter_node); body = compile_block(body_ast)
        def ex(env):
            lv = env.vars
            for item in iterate(itr(env)):
                lv[name] = item
                sig = body(env)
                if sig is not None:
                    if sig is _CONTINUE:
                        continue
                    if sig is _BREAK:
                        break
                    return sig
            return None
        return ex

    if tag == "foreach2":
        _, _, name1, name2, iter_node, body_ast = stmt
        itr = compile_expr(iter_node); body = compile_block(body_ast)
        def ex(env):
            lv = env.vars
            for first, second in iterate_pairs(itr(env)):
                lv[name1] = first
                lv[name2] = second
                sig = body(env)
                if sig is not None:
                    if sig is _CONTINUE:
                        continue
                    if sig is _BREAK:
                        break
                    return sig
            return None
        return ex

    if tag == "repeat":
        cnt = compile_expr(stmt[2]); body = compile_block(stmt[3])
        def ex(env):
            count = cnt(env)
            if not isinstance(count, int) or isinstance(count, bool):
                raise VantaError("repeat needs a whole number of times")
            for _ in range(count):
                sig = body(env)
                if sig is not None:
                    if sig is _CONTINUE:
                        continue
                    if sig is _BREAK:
                        break
                    return sig
            return None
        return ex

    if tag == "func":
        _, _, name, params, body_ast = stmt
        compiled_body = compile_block(body_ast)
        def ex(env):
            fn = Function(name, params, body_ast)
            fn.compiled = compiled_body
            fn.defining_env = env
            env.vars[name] = fn
        return ex

    if tag == "fix":
        name = stmt[2]; v = compile_expr(stmt[3])
        def ex(env):
            env.fix(name, v(env))
        return ex

    if tag == "append":
        v = compile_expr(stmt[2]); t = compile_expr(stmt[3])
        def ex(env):
            value = v(env); target = t(env)
            if not isinstance(target, list):
                raise VantaError("you can only 'add ... to' a list")
            target.append(value)
        return ex

    if tag == "match":
        subj = compile_expr(stmt[2])
        branches = [(compile_expr(vn), compile_block(b)) for vn, b in stmt[3]]
        else_body = compile_block(stmt[4])
        def ex(env):
            s = subj(env)
            for value_node, body in branches:
                if s == value_node(env):
                    return body(env)
            return else_body(env)
        return ex

    if tag == "attempt":
        _, _, body_ast, errname, rescue_ast = stmt
        body = compile_block(body_ast); rescue = compile_block(rescue_ast)
        def ex(env):
            try:
                return body(env)
            except VantaError as e:
                env.vars[errname] = clean_message(str(e))
                return rescue(env)
        return ex

    if tag == "stop":
        return lambda env: _BREAK

    if tag == "skip":
        return lambda env: _CONTINUE

    if tag == "import":
        imp = compile_expr(stmt[2])
        return lambda env: import_file(display(imp(env)))

    # rare statements (type definitions, multi-assign, ask): reuse the proven
    # tree-walker — these run once or seldom and aren't on any hot path.
    return lambda env: _run_stmt(stmt, env)


def compile_block(stmts):
    compiled = [(compile_stmt(s), s[1]) for s in stmts]

    if len(compiled) == 1:
        # the common case (an if-branch, a one-line loop body) — skip the loop
        only, ln = compiled[0]
        def run_one(env):
            try:
                return only(env)
            except VantaError as e:
                if str(e).startswith("line "):
                    raise
                raise VantaError(f"line {ln}: {e}")
        return run_one

    def run(env):
        for ex, lineno in compiled:
            try:
                sig = ex(env)
            except VantaError as e:
                if str(e).startswith("line "):
                    raise
                raise VantaError(f"line {lineno}: {e}")
            if sig is not None:
                return sig          # _Return / _BREAK / _CONTINUE — propagate up
        return None
    return run


# ===========================================================================
# ENTRY POINT
# ===========================================================================

GLOBAL_ENV = Environment()
GLOBAL_ENV.define("pi", math.pi)
GLOBAL_ENV.define("e", math.e)


def reset_runtime():
    """Clear all program state so a host (REPL, IDE, playground) can run a
    fresh program without restarting the interpreter."""
    GLOBAL_ENV.vars.clear()
    if type(GLOBAL_ENV.consts) is not frozenset:
        GLOBAL_ENV.consts.clear()
    GLOBAL_ENV.define("pi", math.pi)
    GLOBAL_ENV.define("e", math.e)
    IMPORTED.clear()


def call_vanta(name, args):
    """Call a Vanta function (or builtin) by name from host code (used by the
    browser playground to drive the emulator). Returns the function's value."""
    fn = GLOBAL_ENV.get(name)
    if fn is _MISSING:
        fn = BUILTIN_VALUES.get(name, _MISSING)
    if fn is _MISSING:
        raise VantaError(f"no function named '{name}'")
    return apply_callable(fn, list(args))


def run_source(source):
    compile_block(parse_program(load_lines(source)))(GLOBAL_ENV)


def repl():
    print(f"Vanta {VERSION} - type Vanta code, or 'bye' to quit.")
    buffer, depth = [], 0
    while True:
        try:
            line = input("...   " if buffer else "vanta> ")
        except EOFError:
            print()
            break
        text = line.strip()
        if not buffer and text in ("bye", "exit", "quit"):
            break
        if not buffer and (text == "" or text.startswith("#")):
            continue
        head = first_word(text)
        if head in BLOCK_OPENERS:
            depth += 1
        elif head == "end" and depth > 0:
            depth -= 1
        buffer.append(text)
        if depth == 0:
            program = "\n".join(buffer)
            buffer = []
            try:
                run_source(program)
            except VantaError as e:
                print(f"Oops! {e}")
            except (BreakLoop, ContinueLoop):
                print("Oops! 'stop'/'skip' only work inside a loop")
            except ReturnSignal:
                print("Oops! 'give back' only works inside a function")


USAGE = """\
Vanta - a plain-English programming language.

  vanta program.va [args]   run a program
  vanta run program.va      run a program (explicit form)
  vanta repl                start the interactive REPL
  vanta version             print the version
  vanta help                show this message
"""


def main():
    global PROGRAM_ARGS
    args = sys.argv[1:]

    if not args:
        repl()
        return

    command = args[0]
    if command in ("help", "--help", "-h"):
        print(USAGE)
        return
    if command in ("version", "--version", "-v"):
        print(f"Vanta {VERSION}")
        return
    if command == "repl":
        repl()
        return
    if command == "run":
        if len(args) < 2:
            print("Usage: vanta run program.va")
            sys.exit(1)
        path, PROGRAM_ARGS = args[1], args[2:]
    else:
        path, PROGRAM_ARGS = args[0], args[1:]

    try:
        with open(path, "r") as f:
            source = f.read()
    except OSError as e:
        print(f"Oops! could not open {path}: {e}")
        sys.exit(1)
    try:
        run_source(source)
    except VantaError as e:
        print(f"Oops! {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
