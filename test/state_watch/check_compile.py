#!/usr/bin/env python3
"""Check the sole state-watch DSL spelling and rejection of retired overloads."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--msvc", action="store_true")
    args = parser.parse_args()
    prefix = '#include <cels/cels.h>\ntypedef struct Settings { int value; } Settings;\n'
    probes = {
        "valid": ('const Settings *Watch(const Settings *settings) { return cel_watch_state(settings); }\n', True),
        "retired_name": ('void Watch(const Settings *settings) { (void)cel_watch(settings); }\n', False),
        "retired_type_lookup": ('void Watch(void) { (void)cel_watch_state(Settings); }\n', False),
        "retired_keyed_watch": ('void Watch(void) { (void)cel_watch_state(Settings, 1); }\n', False),
        "retired_session_watch": ('void Watch(CelsSession *session) { (void)cel_watch_state(session, Settings, 1); }\n', False),
    }
    with tempfile.TemporaryDirectory(prefix="cels-watch-compile-") as directory:
        for name, (body, should_compile) in probes.items():
            source = Path(directory) / f"{name}.c"
            source.write_text(prefix + body, encoding="utf-8")
            flags = (["/nologo", "/std:c11", "/WX", "/Zs", f"/I{args.include}"] if args.msvc else
                     ["-std=c99", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only", "-I", args.include])
            result = subprocess.run([args.compiler, *flags, str(source)], text=True,
                                    capture_output=True, timeout=30, check=False)
            if (result.returncode == 0) != should_compile:
                raise RuntimeError(f"{name}: unexpected compiler result\n{result.stdout}{result.stderr}")
            if not should_compile and "cel_watch" not in result.stdout + result.stderr:
                raise RuntimeError(f"{name}: rejection did not identify the watch API\n{result.stdout}{result.stderr}")
    print("State watch: valid C99 pointer call and four retired spellings checked.")


if __name__ == "__main__":
    main()
