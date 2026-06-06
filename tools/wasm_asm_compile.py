#!/usr/bin/env python3
"""Compile a translated wasm GAS file, splitting into safe-sized chunks.

clang-22's wasm32 integrated assembler segfaults on files larger than ~8900
lines.  This script splits the input at .size boundaries (end of each data
symbol) into chunks of at most CHUNK_MAX_LINES lines, compiles each chunk
with clang, and merges the resulting objects with wasm-ld --relocatable.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

CHUNK_MAX_LINES = 6000


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
    content = input_path.read_text()
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
