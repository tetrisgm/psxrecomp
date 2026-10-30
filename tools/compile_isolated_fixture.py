"""Compile a closed C fixture with main as its only externally visible definition.

Clang/MinGW LTO retains references from unused definitions in tests which include
production .c files. Internalizing the fixture's definitions before optimization
provides the same closed-program dead stripping used by GCC's LTO. Undefined
references remain external: reachable missing implementations still fail to link.
This is only for standalone test executables, never production libraries/runtime.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def compile_fixture(cc, source, output, includes=(), depfile=None):
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="isolated-fixture-", dir=output.parent) as temp:
        ir = Path(temp) / "fixture.ll"
        command = [cc, "-O2", "-S", "-emit-llvm", *["-I" + str(p) for p in includes]]
        if depfile:
            command += ["-MMD", "-MF", str(depfile), "-MT", str(output)]
        subprocess.run([*command, str(source), "-o", str(ir)], check=True)
        text = ir.read_text()
        if not re.search(r"^define .*@main\(", text, re.M):
            raise ValueError("isolated fixture must define main")
        if "dllexport" in text:
            raise ValueError("exported definitions are not a closed test fixture")
        text = re.sub(r"^define (?:dso_local )?(?!internal |private )([^\n]*?@)(?!main\()([^\n]+)",
                      r"define internal \1\2", text, flags=re.M)
        text = re.sub(r"^(@[^\n=]+ = )dso_local ", r"\1internal ", text, flags=re.M)
        ir.write_text(text)
        subprocess.run([cc, "-O2", "-c", str(ir), "-o", str(output)], check=True)


def self_test(cc):
    with tempfile.TemporaryDirectory(prefix="fixture-compiler-test-") as temp:
        root = Path(temp)
        src, obj, exe = root / "test.c", root / "test.obj", root / "test.exe"
        src.write_text("extern int missing(void); int unused(void) { return missing(); }\n"
                       "int live(int n) { return n * 3; } int main(void) { return live(7) != 21; }\n")
        compile_fixture(cc, src, obj)
        subprocess.run([cc, str(obj), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        src.write_text("extern int missing(void); int main(void) { return missing(); }\n")
        compile_fixture(cc, src, obj)
        result = subprocess.run([cc, str(obj), "-o", str(exe)], capture_output=True)
        if result.returncode == 0 or b"missing" not in result.stderr:
            raise AssertionError("reachable unresolved implementation must fail the link")
    print("PASS: unused definitions discarded; live behavior and unresolved failures retained")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--source")
    parser.add_argument("--output")
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--depfile")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test(args.cc)
    else:
        if not args.source or not args.output:
            parser.error("--source and --output are required")
        compile_fixture(args.cc, args.source, args.output, args.include, args.depfile)
