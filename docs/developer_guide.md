# Developer Guide: Integrating CELS with CMake

This guide explains how to add CELS to your C99 project using CMake and configure your applications using the official **`cels_add_application(...)`** helper function.

---

## 1. Adding CELS to Your CMake Project

You can integrate CELS into your project using one of four straightforward methods:

### Option A: Via `FetchContent` (Recommended)
Add CELS directly from GitHub with zero manual installation:

```cmake
cmake_minimum_required(VERSION 3.20...4.3)
project(my_project C)

include(FetchContent)
FetchContent_Declare(
    cels
    GIT_REPOSITORY https://github.com/jackblitz/cels.git
    GIT_TAG v0.1.0 # Or specific commit / branch
)
FetchContent_MakeAvailable(cels)
```

### Option B: Via Release Tarball / Zip
Download an official release archive automatically:

```cmake
include(FetchContent)
FetchContent_Declare(
    cels
    URL https://github.com/jackblitz/cels/archive/refs/tags/v0.1.0.tar.gz
)
FetchContent_MakeAvailable(cels)
```

### Option C: Via Subdirectory or Git Submodule
If you keep CELS in a vendor/submodule folder:

```cmake
add_subdirectory(path/to/cels)
```

### Option D: Via `find_package` (Pre-installed Package)
If CELS was installed on the system (e.g. via `cmake --install`):

```cmake
find_package(Cels CONFIG REQUIRED)
```

> [!NOTE]
> When CELS is consumed as a subproject or dependency via `FetchContent` / `add_subdirectory`, internal unit tests (`test_cli`) and benchmarks (`benchmark`) are **automatically disabled** to keep your build times lightning fast.

---

## 2. Defining Applications with `cels_add_application`

Once CELS is added, you can declare your application targets with a single function call:

```cmake
cels_add_application(
    HOST my_host
    APP my_app
    HOST_SOURCES src/host.c
    APP_SOURCES
        src/app.c
        src/ui_window.c
)
```

### Generated Targets

| Target Name | Type | Purpose |
|---|---|---|
| **`<HOST>`** (e.g. `my_host`) | Executable | The primary host engine process (`.exe`). |
| **`<APP>`** (e.g. `my_app`) | Shared Library / Alias | The application logic module (`.dll` in Debug, alias in Release). |
| **`<APP>_rebuild`** (e.g. `my_app_rebuild`) | Executable Runner | Rebuild runner target for IDEs. Allows clicking **Play** (<kbd>Shift</kbd>+<kbd>F10</kbd>) to rebuild and hot-reload. |

### Supported Modes

| Mode | Behavior | Use Case |
|---|---|---|
| **`AUTO`** *(Default)* | Debug builds generate **`.exe` + `.dll`** with live hot-reload.<br>Release builds generate a **single monolithic `.exe`**. | Recommended for standard development workflows. |
| **`SINGLE_BINARY`**<br>*(or `MONOLITHIC`)* | Always compiles host and app sources into a **single standalone `.exe`** with zero `.dll` dependencies and `CELS_HOT_RELOAD=0`. | Production shipping, embedded devices, static distributions. |
| **`HOT_RELOAD`** | Always compiles a host `.exe` and an app shared library (`.dll` / `.so` / `.dylib`) with `add_dependencies(${HOST} ${APP})` and `CELS_HOT_RELOAD=1`. | Dynamic plugin workflows, live code-patching environments. |

---

## 3. Explicit Mode Examples

### Monolithic Standalone Executable (Single Binary)
```cmake
cels_add_application(
    HOST my_standalone_host
    APP my_standalone_app
    HOST_SOURCES src/host.c
    APP_SOURCES src/app.c
    MODE SINGLE_BINARY
)
```
- Compiles both `host.c` and `app.c` directly into `my_standalone_host.exe`.
- Defines `CELS_HOT_RELOAD=0`.
- Automatically generates compatibility targets for `my_standalone_app` and `my_standalone_app_rebuild` so IDE configurations remain valid.

### Dynamic Hot-Reload Host & DLL
```cmake
cels_add_application(
    HOST my_host
    APP my_app
    HOST_SOURCES src/host.c
    APP_SOURCES src/app.c
    MODE HOT_RELOAD
)
```
- Generates `my_host.exe` and `my_app.dll` co-located in the same directory.
- Adds `add_dependencies(my_host my_app)` so launching `my_host` automatically recompiles `my_app.dll` if needed.
- Defines `CELS_HOT_RELOAD=1`, `CELS_CMAKE_COMMAND`, `CELS_BINARY_DIR`, and `CELS_APP_TARGET="my_app"`.
- Generates the `my_app_rebuild` executable runner and terminal scripts.

---

## 4. Developer Hot-Reloading Workflows (Debug Mode)

In Debug mode, the host engine (`my_host.exe`) runs continuously while watching the application library (`my_app.dll`). When the library changes on disk, CELS uses an automatic shadow-copy mechanism to swap the library with **zero Windows DLL file locking**, preserving existing state (`WindowState`, remembered counters) in **<50ms**.

### 5 Ways to Rebuild Your Application During Development

Whenever you edit your composables or UI logic:

#### Option 1: In CLion / IDEs (Play Button)
1. Select **`<app>_rebuild`** (e.g. `my_app_rebuild`) in the top-right configuration dropdown.
2. Click the green **Play** button (<kbd>Shift</kbd>+<kbd>F10</kbd>).
3. The generated runner invokes CMake to rebuild the DLL. If `my_host` is running, it reloads instantly!

#### Option 2: In CLion / IDEs (Build Shortcut)
1. Select **`<app>`** (e.g. `my_app`) in the configuration dropdown.
2. Press <kbd>Ctrl</kbd>+<kbd>F9</kbd> (or click the Hammer icon).

#### Option 3: Terminal Helper Scripts
`cels_add_application` automatically creates ready-to-run rebuild scripts in your build directory:
- **Windows**: `.\build\rebuild_<app>.bat`
- **Linux / macOS**: `./build/rebuild_<app>.sh`

#### Option 4: Console Hotkey
Press `[r]` or `[b]` directly inside the running host terminal window. The host engine executes CMake in the background and reloads automatically.

#### Option 5: Automatic on Host Launch
Whenever you launch `<host>` (<kbd>Shift</kbd>+<kbd>F10</kbd>), CMake target dependencies ensure `<app>.dll` is compiled freshly before the host starts.

---

## 5. Shipping Production Distributions (Release Mode)

For production distributions, build in Release mode to produce a single, self-contained executable with zero DLL dependencies:

```powershell
# Configure in Release mode
cmake -B build-release -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build-release --target my_host

# Run single standalone binary
.\build-release\my_host.exe
```

When building in Release mode:
- All composables and host engine code compile directly into `my_host.exe`.
- `CELS_HOT_RELOAD=0` is set, eliminating all dynamic loading and file watching overhead.
- There are **zero `.dll` dependencies** to ship or package.

---

## 6. Full Parameter Reference

```cmake
cels_add_application(
    HOST <target_name>                  # [Required] Name of the host executable target
    APP <target_name>                   # [Required] Name of the app dynamic library target
    HOST_SOURCES <sources...>           # [Required] Host engine entry point sources
    APP_SOURCES <sources...>            # [Required] Composable tree and application logic sources
    [MODE <AUTO|HOT_RELOAD|SINGLE_BINARY|MONOLITHIC>] # Build mode (default: AUTO)
    [HOT_RELOAD]                        # Boolean convenience flag for MODE HOT_RELOAD
    [SINGLE_BINARY]                     # Boolean convenience flag for MODE SINGLE_BINARY
    [MONOLITHIC]                        # Synonym for SINGLE_BINARY
    [INCLUDES <dirs...>]                # Common include directories for both host and app
    [HOST_INCLUDES <dirs...>]           # Include directories for host target only
    [APP_INCLUDES <dirs...>]            # Include directories for app target only
    [LIBRARIES <libs...>]               # Extra libraries to link to both targets
    [HOST_LIBRARIES <libs...>]          # Extra libraries to link to host target only
    [APP_LIBRARIES <libs...>]           # Extra libraries to link to app target only
    [DEFINES <defs...>]                 # Preprocessor compile definitions
    [OUTPUT_DIR <dir>]                  # Output directory (defaults to current binary dir)
)
```
