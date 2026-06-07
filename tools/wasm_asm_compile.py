#!/usr/bin/env python3
"""Compile a translated wasm GAS file, splitting into safe-sized chunks.

clang-22's wasm32 integrated assembler segfaults on files larger than ~8900
lines.  This script splits the input at .size boundaries (end of each data
symbol) into chunks of at most CHUNK_MAX_LINES lines, compiles each chunk
with clang, and merges the resulting objects with wasm-ld --relocatable.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CHUNK_MAX_LINES = 6000

# A label definition on its own line, e.g. "LittlerootTown:".
_LABEL_DEF = re.compile(r"^([A-Za-z_][A-Za-z0-9_.$]*):\s*$")


def globalize_labels(content: str) -> str:
    """Emit a `.globl` for every locally-defined data label.

    The file is split into separately-compiled chunks (see split_chunks) that
    are merged with `wasm-ld --relocatable`. A symbol that is referenced from a
    different chunk than the one defining it must be global, otherwise the
    reference resolves to 0 at merge time. Most generated data labels (e.g. the
    per-map MapHeader structs `LittlerootTown`, `InsideOfTruck`) are local, so
    any cross-chunk `.4byte <label>` silently became a null pointer. Promoting
    every definition to global keeps chunking transparent.
    """
    out: list[str] = []
    already_global: set[str] = set()
    for line in content.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith(".globl"):
            already_global.add(stripped.split(None, 1)[1].strip())
        match = _LABEL_DEF.match(stripped)
        if match and match.group(1) not in already_global:
            out.append(f".globl {match.group(1)}\n")
            already_global.add(match.group(1))
        out.append(line)
    return "".join(out)


def split_chunks(content: str) -> list[str]:
    lines = content.splitlines(keepends=True)
    chunks: list[str] = []
    current: list[str] = []
    section_header = '.section .rodata,"",@\n'

    for line in lines:
        stripped = line.strip()
        if stripped.startswith(".section"):
            section_header = line
        current.append(line)
        if stripped.startswith(".size ") and len(current) >= CHUNK_MAX_LINES:
            chunks.append("".join(current))
            current = [section_header]

    if current:
        chunks.append("".join(current))
    return chunks


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: wasm_asm_compile.py <input.wasm.s> <output.o>")

    input_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    content = globalize_labels(input_path.read_text())
    chunks = split_chunks(content)

    clang = ["clang", "--target=wasm32-unknown-unknown", "-x", "assembler"]

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        if len(chunks) == 1:
            subprocess.run(
                clang + [str(input_path), "-c", "-o", str(output_path)],
                check=True,
            )
            return

        obj_files: list[str] = []
        for i, chunk in enumerate(chunks):
            chunk_s = tmp / f"chunk_{i}.s"
            chunk_o = tmp / f"chunk_{i}.o"
            chunk_s.write_text(chunk)
            subprocess.run(
                clang + [str(chunk_s), "-c", "-o", str(chunk_o)],
                check=True,
            )
            obj_files.append(str(chunk_o))

        subprocess.run(
            ["wasm-ld", "--relocatable", "-o", str(output_path)] + obj_files,
            check=True,
        )


if __name__ == "__main__":
    main()
