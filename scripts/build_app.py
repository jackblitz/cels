#!/usr/bin/env python3
"""
CELS Application Rebuild Script
Universal cross-platform build script for Windows, Linux, and macOS.
"""

import os
import sys
import subprocess
import shutil
from pathlib import Path

def main():
    script_dir = Path(__file__).resolve().parent
    root_dir = script_dir.parent

    print("======================================================================")
    print("  [CELS] Building Application Library (Universal Python Script)")
    print("======================================================================")

    if len(sys.argv) > 1:
        build_dir = Path(sys.argv[1]).resolve()
    else:
        candidates = [
            root_dir / "cmake-build-debug",
            root_dir / "build",
        ]
        build_dir = next((c for c in candidates if c.exists()), root_dir / "cmake-build-debug")

    cmake_bin = shutil.which("cmake")
    if not cmake_bin:
        if os.name == "nt":
            common_clion = Path("C:/Program Files/JetBrains")
            for path in common_clion.glob("CLion*/bin/cmake/win/x64/bin/cmake.exe"):
                if path.exists():
                    cmake_bin = str(path)
                    break
        elif sys.platform == "darwin":
            mac_clion = Path("/Applications/CLion.app/Contents/bin/cmake/mac/bin/cmake")
            if mac_clion.exists():
                cmake_bin = str(mac_clion)

    if not cmake_bin:
        cmake_bin = "cmake"

    print(f"[CELS] Using CMake : {cmake_bin}")
    print(f"[CELS] Build Dir   : {build_dir}")
    print(f"[CELS] Target      : cel_app\n")

    cmd = [cmake_bin, "--build", str(build_dir), "--target", "cel_app"]
    res = subprocess.run(cmd)

    if res.returncode == 0:
        print("\n======================================================================")
        print("  [CELS] Application library successfully updated!")
        print("======================================================================")
    else:
        print(f"\n[CELS] Error: Build failed with exit code {res.returncode}")

    sys.exit(res.returncode)

if __name__ == "__main__":
    main()
