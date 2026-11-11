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
        "valid": ('const Settings *Watch(const Settings *settings) { return Cel.State.Watch(settings); }\n', True),
        "embedding": ('#undef Cel\n#define CELS_NO_SERVICE_DSL\nconst void *Watch(const Settings *settings) { return CelsApiV1()->State.Watch(settings); }\n', True),
        "retired_pointer_watch": ('void Watch(const Settings *settings) { (void)cel_watch_state(settings); }\n', False),
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
    with tempfile.TemporaryDirectory(prefix="cels-service-compile-") as directory:
        for name, source, valid in (
            ("immutable", '#include <cels/api.h>\nvoid Invalid(void) { Cel.State.Watch = 0; }\n', False),
            ("embed_header", '#define CELS_NO_SERVICE_DSL\n#include <cels/api.h>\n#ifdef Cel\n#error Cel must be supplied by the embedding framework\n#endif\nconst void *Watch(const void *p) { return CelsApiV1()->State.Watch(p); }\n', True),
        ):
            path = Path(directory) / f"{name}.c"
            path.write_text(source, encoding="utf-8")
            result = subprocess.run([args.compiler, *flags, str(path)], text=True,
                                    capture_output=True, timeout=30, check=False)
            if (result.returncode == 0) != valid:
                raise RuntimeError(f"{name}: unexpected compiler result\n{result.stdout}{result.stderr}")
            if not valid and "Watch" not in result.stdout + result.stderr:
                raise RuntimeError(f"{name}: wrong rejection reason")
    print("State watch: valid C99 pointer call and five retired spellings checked.")


if __name__ == "__main__":
    main()
