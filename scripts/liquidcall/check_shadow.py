#!/usr/bin/env python3
"""Flag a name declared again in a nested block.

Apple clang builds liquid_coreml.mm with -Wshadow -Werror. Linux CI does not
compile that file. This scan is the same class of mistake: a local such as
`shape` declared again inside a loop.
"""
import re
import sys
from pathlib import Path

# A local, not a function. `shape =` and `shape;` count. `DescribeLatency(` does not.
DECL = re.compile(
    r"^\s*(?:(?:const|constexpr|static)\s+)*"
    r"(?:[\w:<>*&]+\s+)+([A-Za-z_]\w*)\s*(?:[=;,])"
)
SKIP_FIRST = {
    "if", "for", "while", "switch", "return", "catch", "else", "do",
    "class", "struct", "enum", "namespace", "operator", "case",
}


def strip_noise(text: str) -> str:
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            if j < 0:
                break
            out.append("\n")
            i = j + 1
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            chunk = text[i:j + 2] if j >= 0 else text[i:]
            out.append(re.sub(r"[^\n]", " ", chunk))
            i = j + 2 if j >= 0 else n
            continue
        if text[i] in "\"'":
            quote = text[i]
            out.append(" ")
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def check(path: Path) -> list[str]:
    text = strip_noise(path.read_text())
    scopes = [set()]
    errors = []
    for lineno, line in enumerate(text.splitlines(), 1):
        first = line.split(None, 1)[0] if line.split() else ""
        match = DECL.match(line)
        if match and scopes and first not in SKIP_FIRST:
            name = match.group(1)
            # File and namespace scope hold function names. -Wshadow here is a
            # local hiding another local, which is what Apple clang rejected.
            if len(scopes) > 1:
                for outer in scopes[1:-1]:
                    if name in outer:
                        errors.append(
                            f"{path}:{lineno}: '{name}' shadows an outer local"
                        )
                        break
            scopes[-1].add(name)
        for ch in line:
            if ch == "{":
                scopes.append(set())
            elif ch == "}" and len(scopes) > 1:
                scopes.pop()
    return errors


def main() -> int:
    sample = (
        "void f() {\n"
        "  std::vector<int> shape;\n"
        "  if (true) {\n"
        "    const std::vector<int> shape = 1;\n"
        "  }\n"
        "}\n"
    )
    probe = Path(__file__).with_name("_shadow_probe.cc")
    probe.write_text(sample)
    try:
        found = check(probe)
    finally:
        probe.unlink(missing_ok=True)
    if not any("shape" in item for item in found):
        print("shadow checker did not catch a nested shape", file=sys.stderr)
        return 1
    root = Path(__file__).resolve().parents[2]
    # Only the Objective-C++ file. Apple clang compiles it with -Wshadow and
    # Linux does not. The portable .cc files are compiled with -Wshadow below.
    files = list((root / "modules/audio_coding/codecs/liquidcall").glob("*.mm"))
    errors = []
    for path in files:
        errors.extend(check(path))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"no shadowed locals in {len(files)} liquidcall sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
