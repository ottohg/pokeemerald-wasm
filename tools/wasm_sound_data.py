#!/usr/bin/env python3
"""Translate m4a/MIDI sound assembly into clang-wasm-assemblable GAS.

The GBA sound data (data/sound_data.s and the mid2agb song .s files) relies on
GNU-as features that LLVM's wasm integrated assembler rejects:

  * ``.set`` redefinition (used by the keysplit/split macros),
  * location-counter arithmetic (``.set voicegroup_x, . - n * 0xC``),
  * ``.rept``/``.endm`` macros and ``@`` line comments.

This tool acts as a tiny macro-expanding assembler front end: it inlines
``.include``s, expands the m4a/music_voice macros, evaluates ``.set``/``.equ``
constants, resolves location-counter aliases against a per-section byte counter
(anchored to a real symbol so wasm-ld relocations stay correct), and emits flat
``.byte/.2byte/.4byte/.incbin`` plus ``.size`` that clang can assemble.

Usage:  wasm_sound_data.py <input.s> <output.s>
"""
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]
INCLUDE_DIRS = [ROOT, ROOT / "sound", ROOT / "include"]

IDENT = r"[A-Za-z_.$][\w.$]*"
LABEL_RE = re.compile(rf"^({IDENT})(::?)\s*(.*)$")
ASSIGN_RE = re.compile(rf"^({IDENT})\s*=\s*(.+)$")


class Macro:
    __slots__ = ("params", "body")

    def __init__(self, params: List[Tuple[str, Optional[str]]], body: List[str]):
        self.params = params  # (name, default) ; default is "" for :req, None otherwise
        self.body = body


def strip_comment(line: str) -> str:
    """Remove @ and // comments (and trailing whitespace), respecting strings."""
    out = []
    in_str = False
    escaped = False
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if escaped:
            out.append(c)
            escaped = False
        elif c == "\\":
            out.append(c)
            escaped = True
        elif c == '"':
            in_str = not in_str
            out.append(c)
        elif not in_str and c == "@":
            break
        elif not in_str and c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        else:
            out.append(c)
        i += 1
    return "".join(out).rstrip()


def resolve_include(name: str, current: Path) -> Path:
    candidates = [current.parent / name] + [d / name for d in INCLUDE_DIRS]
    for cand in candidates:
        if cand.exists():
            return cand
    raise FileNotFoundError(f"cannot resolve .include {name!r} from {current}")


def flatten(path: Path, seen=None) -> List[str]:
    """Inline .include directives, returning a flat list of comment-stripped lines.

    .macro bodies and .incbin paths are preserved verbatim.
    """
    lines: List[str] = []
    text = path.read_text(errors="ignore")
    # Drop /* ... */ block comments (rare, single or multi line).
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    for raw in text.splitlines():
        line = strip_comment(raw)
        if not line:
            continue
        stripped = line.strip()
        m = re.match(r'^\.include\s+"([^"]+)"', stripped)
        if m:
            lines.extend(flatten(resolve_include(m.group(1), path)))
            continue
        lines.append(line)
    return lines


def split_args(text: str) -> List[str]:
    text = text.strip()
    if not text:
        return []
    args, cur, depth = [], [], 0
    for ch in text:
        if ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
            continue
        if ch in "([":
            depth += 1
        elif ch in ")]" and depth:
            depth -= 1
        cur.append(ch)
    args.append("".join(cur).strip())
    return args


def extract_macros(lines: List[str]) -> Tuple[Dict[str, Macro], List[str]]:
    macros: Dict[str, Macro] = {}
    out: List[str] = []
    i = 0
    while i < len(lines):
        s = lines[i].strip()
        if s.startswith(".macro "):
            sig = s[len(".macro "):].strip()
            name, _, params_text = sig.partition(" ")
            params = []
            for p in split_args(params_text):
                if not p:
                    continue
                if p.endswith(":vararg"):
                    params.append((p[:-7].strip(), "__VARARG__"))
                elif p.endswith(":req"):
                    params.append((p[:-4].strip(), ""))
                elif "=" in p:
                    nm, df = p.split("=", 1)
                    params.append((nm.strip(), df.strip()))
                else:
                    params.append((p.strip(), None))
            body = []
            i += 1
            depth = 1
            while i < len(lines):
                bs = lines[i].strip()
                if bs.startswith(".macro "):
                    depth += 1
                elif bs == ".endm":
                    depth -= 1
                    if depth == 0:
                        break
                body.append(lines[i])
                i += 1
            macros[name] = Macro(params, body)
        else:
            out.append(lines[i])
        i += 1
    return macros, out


class Assembler:
    def __init__(self, macros: Dict[str, Macro]):
        self.macros = macros
        self.consts: Dict[str, int] = {}
        self.aliases: Dict[str, str] = {}
        self.out: List[str] = []
        self.section = ".text"
        self.loc: Dict[str, Optional[int]] = {}
        self.anchor: Dict[str, str] = {}
        self.open_label: Optional[str] = None
        self.local_id = 0

    # ---- expression handling -------------------------------------------------
    def expand_aliases(self, expr: str) -> str:
        if not self.aliases:
            return expr
        for _ in range(8):
            new = re.sub(IDENT, lambda m: self.aliases.get(m.group(0), m.group(0)), expr)
            if new == expr:
                break
            expr = new
        return expr

    def subst_tokens(self, expr: str) -> str:
        """Expand symbol aliases, then replace known constants with their value."""
        expr = self.expand_aliases(expr)

        def repl(m):
            tok = m.group(0)
            if tok in self.consts:
                return str(self.consts[tok])
            return tok
        return re.sub(IDENT, repl, expr)

    def try_eval(self, expr: str) -> Optional[int]:
        e = self.subst_tokens(expr).strip()
        if not e:
            return None
        if not re.fullmatch(r"[0-9xXa-fA-F\s()+\-*/%<>&|~^]+", e):
            return None
        try:
            return int(eval(e, {"__builtins__": {}}, {}))
        except Exception:
            return None

    def eval_int(self, expr: str) -> int:
        v = self.try_eval(expr)
        if v is None:
            raise ValueError(f"cannot evaluate integer expr: {expr!r} (consts missing)")
        return v

    def eval_with_dot(self, expr: str) -> int:
        sec = self.section
        loc = self.loc.get(sec)
        if loc is None:
            raise ValueError(f"location counter unknown in {sec} for {expr!r}")
        e = re.sub(r"(?<![\w.])\.(?![\w])", str(loc), expr)
        return self.eval_int(e)

    def format_operand(self, expr: str) -> str:
        """Substitute known consts; fold to int if fully numeric, else keep symbolic."""
        v = self.try_eval(expr)
        if v is not None:
            return str(v)
        return self.subst_tokens(expr).strip()

    # ---- emission ------------------------------------------------------------
    def advance(self, nbytes: Optional[int]):
        sec = self.section
        cur = self.loc.get(sec)
        if cur is None or nbytes is None:
            self.loc[sec] = None
        else:
            self.loc[sec] = cur + nbytes

    def close_label(self):
        if self.open_label is not None:
            self.out.append(f"\t.size {self.open_label}, .-{self.open_label}")
            self.open_label = None

    def ensure_anchor(self):
        sec = self.section
        if sec not in self.anchor:
            # Use .L prefix so the anchor is local to this object (avoids duplicate
            # symbol errors when multiple sound objects share the same section name).
            name = ".Lwsd_anchor_" + re.sub(r"\W", "_", sec.lstrip("."))
            self.anchor[sec] = name
            self.loc.setdefault(sec, 0)
            self.out.append(f"\t.type {name},@object")
            self.out.append(f"{name}:")
        return self.anchor[sec]

    def emit_section(self, name: str):
        self.close_label()
        self.section = name
        if name not in self.loc:
            self.loc[name] = 0
        self.out.append(f'\t.section {name},"",@')
        self.ensure_anchor()

    # ---- main processing -----------------------------------------------------
    def process(self, lines: List[str], bindings: Optional[Dict[str, str]] = None):
        i = 0
        while i < len(lines):
            raw = lines[i]
            i += 1
            if bindings:
                raw = self.apply_bindings(raw, bindings)
            line = raw.strip()
            if not line:
                continue

            # Conditionals --------------------------------------------------
            if line.startswith((".if", ".ifb", ".ifnb", ".ifdef", ".ifndef", ".ifeq", ".ifne")):
                j, taken = self.handle_conditional(lines, i - 1, bindings)
                # handle_conditional processed the active branch already
                i = j
                continue
            if line.startswith((".else", ".elseif", ".endif")):
                # Should be consumed by handle_conditional; stray -> ignore.
                continue

            # Repetition ----------------------------------------------------
            if line.startswith(".rept"):
                count = self.eval_int(line[len(".rept"):].strip())
                body, j = self.collect_block(lines, i, ".rept", ".endr")
                for _ in range(count):
                    self.process(body, bindings)
                i = j
                continue
            if line == ".endr":
                continue

            # Macro definitions inside expansion (skip) ---------------------
            if line.startswith(".macro"):
                _, j = self.collect_block(lines, i, ".macro", ".endm")
                i = j
                continue
            if line == ".endm":
                continue

            self.handle_line(line)
        return

    def collect_block(self, lines, start, open_kw, close_kw):
        body, depth, j = [], 1, start
        while j < len(lines):
            s = lines[j].strip()
            if s.startswith(open_kw):
                depth += 1
            elif s == close_kw or s.startswith(close_kw):
                depth -= 1
                if depth == 0:
                    return body, j + 1
            body.append(lines[j])
            j += 1
        return body, j

    def handle_conditional(self, lines, start, bindings):
        """Process an .if/.else/.endif chain, executing only the active branch."""
        # Build the branch structure at this nesting level.
        branches = []  # list of (condition_line, body_lines)
        i = start
        depth = 0
        cur_cond = lines[i].strip()
        cur_body: List[str] = []
        i += 1
        end = i
        while i < len(lines):
            s = lines[i].strip()
            if s.startswith((".if", ".ifb", ".ifnb", ".ifdef", ".ifndef", ".ifeq", ".ifne")):
                depth += 1
                cur_body.append(lines[i])
            elif depth > 0 and s == ".endif":
                depth -= 1
                cur_body.append(lines[i])
            elif depth == 0 and (s.startswith(".else") or s.startswith(".elseif")):
                branches.append((cur_cond, cur_body))
                cur_cond = s
                cur_body = []
            elif depth == 0 and s == ".endif":
                branches.append((cur_cond, cur_body))
                end = i + 1
                break
            else:
                cur_body.append(lines[i])
            i += 1
        else:
            branches.append((cur_cond, cur_body))
            end = i

        for cond, body in branches:
            if self.cond_true(cond, bindings):
                self.process(body, bindings)
                break
        return end, None

    def cond_true(self, cond: str, bindings) -> bool:
        if bindings:
            cond = self.apply_bindings(cond, bindings)
        cond = cond.strip()
        if cond.startswith(".ifb"):
            return self.apply_bindings(cond[len(".ifb"):], bindings or {}).strip() == ""
        if cond.startswith(".ifnb"):
            return self.apply_bindings(cond[len(".ifnb"):], bindings or {}).strip() != ""
        if cond == ".else":
            return True
        if cond.startswith(".elseif"):
            cond = ".if" + cond[len(".elseif"):]
        if cond.startswith(".ifdef"):
            return cond[len(".ifdef"):].strip() in self.consts
        if cond.startswith(".ifndef"):
            return cond[len(".ifndef"):].strip() not in self.consts
        if cond.startswith(".ifeq"):
            return (self.try_eval(cond[len(".ifeq"):]) or 0) == 0
        if cond.startswith(".ifne"):
            return (self.try_eval(cond[len(".ifne"):]) or 0) != 0
        if cond.startswith(".if"):
            expr = cond[len(".if"):].strip()
            expr = expr.replace("&&", " and ").replace("||", " or ")
            expr = re.sub(r"(?<![=!<>])!(?!=)", " not ", expr)
            expr = self.subst_tokens(expr)
            try:
                return bool(eval(expr, {"__builtins__": {}}, {}))
            except Exception:
                return False
        return True

    def apply_bindings(self, line: str, bindings: Dict[str, str]) -> str:
        if "\\" not in line:
            return line
        for name, val in sorted(bindings.items(), key=lambda kv: -len(kv[0])):
            line = line.replace("\\" + name, val)
        line = line.replace("\\@", str(self.local_id))
        line = line.replace("\\()", "")
        return line

    def handle_line(self, line: str):
        # Label (optionally with trailing statement)
        m = LABEL_RE.match(line)
        if m and (m.group(2) or self.looks_like_label(line)):
            name, colons, rest = m.groups()
            self.emit_label(name, glob=(colons == "::"))
            if rest.strip():
                self.handle_line(rest.strip())
            return

        directive = line.split()[0] if line.split() else ""
        rest = line[len(directive):].strip()

        if directive in (".section",):
            self.emit_section(rest.split(",")[0].strip())
            return
        if directive == ".text":
            self.emit_section(".text")
            return
        if directive == ".rodata":
            self.emit_section(".rodata")
            return
        if directive in (".data",):
            self.emit_section(".data")
            return
        if directive == ".bss":
            self.emit_section(".bss")
            return
        if directive in (".global", ".globl"):
            self.out.append(f"\t.globl {rest}")
            return
        if directive in (".align", ".p2align", ".balign"):
            self.emit_align(directive, rest)
            return
        if directive in (".equ", ".equiv", ".set"):
            self.emit_set(rest)
            return
        if directive in (".byte", ".2byte", ".hword", ".short", ".4byte", ".word", ".long"):
            self.emit_data(directive, rest)
            return
        if directive in (".space", ".skip", ".zero"):
            self.emit_space(rest)
            return
        if directive == ".incbin":
            self.emit_incbin(rest)
            return
        if directive in (".size", ".type"):
            return  # regenerated by us
        if directive in (".end", ".func", ".endfunc", ".thumb", ".arm", ".syntax",
                         ".cpu", ".fpu", ".code", ".thumb_func", ".arm_func",
                         ".purgem", ".error", ".warning", ".ltorg", ".pool",
                         ".eabi_attribute", ".file", ".ident"):
            return
        if directive in (".ascii", ".asciz", ".string"):
            self.emit_string(directive, rest)
            return

        # Assignment with '=' ?
        am = ASSIGN_RE.match(line)
        if am:
            self.emit_set(f"{am.group(1)}, {am.group(2)}")
            return

        # Macro invocation
        name = directive
        if name in self.macros:
            self.expand_macro(name, rest)
            return

        raise ValueError(f"unhandled line: {line!r}")

    def looks_like_label(self, line: str) -> bool:
        # A bare "foo:" with single colon already matched; treat tokens ending ':'
        return bool(re.match(rf"^{IDENT}:", line))

    def emit_label(self, name: str, glob: bool):
        self.close_label()
        if glob:
            self.out.append(f"\t.globl {name}")
        self.out.append(f"\t.type {name},@object")
        self.out.append(f"{name}:")
        self.open_label = name

    def emit_align(self, directive: str, rest: str):
        arg = rest.split(",")[0].strip()
        n = self.eval_int(arg) if arg else 0
        if directive == ".balign":
            p2 = max(0, (n - 1).bit_length()) if n > 1 else 0
            align = n
        else:  # .align / .p2align  -> power of two
            p2 = n
            align = 1 << n
        self.out.append(f"\t.p2align {p2}")
        cur = self.loc.get(self.section)
        if cur is not None and align > 0:
            self.loc[self.section] = (cur + align - 1) // align * align

    def emit_set(self, rest: str):
        name, _, expr = rest.partition(",")
        name = name.strip()
        expr = expr.strip()
        if re.search(r"(?<![\w.])\.(?![\w])", expr):
            # Location-counter alias: bind to anchor + offset.
            anchor = self.ensure_anchor()
            value = self.eval_with_dot(expr)
            if value >= 0:
                self.out.append(f"\t.set {name}, {anchor} + {value}")
            else:
                self.out.append(f"\t.set {name}, {anchor} - {-value}")
            self.out.append(f"\t.globl {name}")
        else:
            v = self.try_eval(expr)
            if v is not None:
                self.consts[name] = v
            else:
                # Alias to a relocatable symbol (e.g. .equ song_grp, voicegroup_x).
                # Emitting `.set name, extern` makes clang crash (no section), so
                # record it as a text alias substituted at every use site instead.
                self.aliases[name] = self.subst_tokens(expr).strip()

    def emit_data(self, directive: str, rest: str):
        size = {".byte": 1, ".2byte": 2, ".hword": 2, ".short": 2,
                ".4byte": 4, ".word": 4, ".long": 4}[directive]
        ops = split_args(rest)
        formatted = [self.format_operand(op) for op in ops]
        out_dir = {".word": ".4byte", ".long": ".4byte", ".hword": ".2byte",
                   ".short": ".2byte"}.get(directive, directive)
        self.out.append(f"\t{out_dir} " + ", ".join(formatted))
        self.advance(size * len(ops))

    def emit_space(self, rest: str):
        ops = split_args(rest)
        n = self.eval_int(ops[0])
        self.out.append(f"\t.space {n}")
        self.advance(n)

    def emit_incbin(self, rest: str):
        ops = split_args(rest)
        path = ops[0].strip().strip('"')
        full = (ROOT / path)
        self.out.append(f'\t.incbin "{path}"')
        if len(ops) >= 3:
            self.advance(self.try_eval(ops[2]))
        elif full.exists():
            self.advance(full.stat().st_size)
        else:
            self.advance(None)

    def emit_string(self, directive: str, rest: str):
        self.out.append(f"\t{directive} {rest}")
        self.advance(None)  # size varies; no alias depends on it

    def expand_macro(self, name: str, arg_text: str):
        macro = self.macros[name]
        self.local_id += 1
        args = split_args(arg_text)
        bindings: Dict[str, str] = {}
        positional = []
        keyword = {}
        for a in args:
            mkw = re.match(rf"^({IDENT})\s*=\s*(.*)$", a)
            if mkw and mkw.group(1) in {p[0] for p in macro.params}:
                keyword[mkw.group(1)] = mkw.group(2)
            else:
                positional.append(a)
        pi = 0
        for pname, default in macro.params:
            if default == "__VARARG__":
                bindings[pname] = ", ".join(positional[pi:])
                pi = len(positional)
            elif pname in keyword:
                bindings[pname] = keyword[pname]
            elif pi < len(positional):
                bindings[pname] = positional[pi]
                pi += 1
            elif default is not None:
                bindings[pname] = default
            else:
                bindings[pname] = ""
        self.process(macro.body, bindings)

    def finish(self):
        self.close_label()
        # Size the anchors so they are valid referenced data symbols.
        for sec, name in self.anchor.items():
            self.out.append(f"\t.size {name}, 0")


def convert(input_path: Path) -> str:
    lines = flatten(input_path)
    macros, body = extract_macros(lines)
    asm = Assembler(macros)
    # Default section for files that emit data before any .section.
    asm.process(body)
    asm.finish()
    header = (
        "# Generated by tools/wasm_sound_data.py - do not edit.\n"
        f"# source: {input_path.relative_to(ROOT) if input_path.is_relative_to(ROOT) else input_path}\n"
    )
    return header + "\n".join(asm.out) + "\n"


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: wasm_sound_data.py <input.s> <output.s>")
    src = Path(sys.argv[1]).resolve()
    dst = Path(sys.argv[2])
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(convert(src))


if __name__ == "__main__":
    main()
