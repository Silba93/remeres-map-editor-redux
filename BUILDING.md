# Building RME

## Prerequisites

| Tool | Windows | Linux (Ubuntu) |
|------|---------|----------------|
| C++ Compiler | Visual Studio 2026 (2022 supported) | GCC 11+ or Clang 14+ |
| CMake | 3.28+ | 3.28+ |
| Package Manager | vcpkg | Conan 2.x |

---

## Windows Setup

### Open, build, and run in Visual Studio

Install Visual Studio 2026 with the **Desktop development with C++** workload.

1. Double-click `Open in Visual Studio.bat`.
2. If the generated solution is missing, open the repository folder in Visual Studio and select the `vs2026-vcpkg` CMake configure preset once.
3. In Visual Studio, choose **Debug** or **Release** from the configuration selector and choose **rme** as the startup target if it is not already selected.
4. Click the green **Play** button (or press F5). Visual Studio builds the selected configuration and launches the editor.

The generated solution is `build-vs2026/rme.slnx`. It and all intermediate build output are disposable; CMake recreates them from the checked-in project files. vcpkg packages are kept in the ignored root `vcpkg_installed/` directory so Debug and Release share the installed dependencies.

To configure without opening Visual Studio, set `VCPKG_ROOT` to the vcpkg directory inside the Visual Studio installation and run `cmake --preset vs2026-vcpkg` from the repository root.

---
## Linux (Ubuntu) Setup

### 1. Install Dependencies
You can use the optimized setup script which handles both `apt` and `Conan` dependencies:

```bash
chmod +x setup_conan.sh
./setup_conan.sh
```

> [!NOTE]
> This script is optimized for **Jules AI** and low-resource environments. It uses local gitignored directories for logs and ensures that only necessary binary libraries are downloaded.
> Since Linux only fetches a few small libraries from Conan (the rest are from `apt`), it uses `--build=missing` to guarantee a successful run without wasting time.

### 2. Build

#### Option A: Optimized Clang Build (Recommended for AI/Jules)
This is the fastest build path, using `Clang`, `Ninja`, and `Mold`. It is specifically optimized to run on 8GB RAM without timing out.

```bash
chmod +x build_clang.sh
./build_clang.sh
```
- **Mode:** Debug (`-O0 -g0` for maximum speed)
- **Parallelism:** Limited to 2 jobs to prevent OOM.
- **Output:** `build_clang/build/Debug/rme`
- **Log:** `build_clang/build_clang.log` (Errors are also printed to screen)

#### Option B: Standard GCC Build
```bash
chmod +x linux_build.sh
./linux_build.sh
```
- **Output:** `linux_build/build/Release/rme`
- **Log:** `linux_build/build_linux.log`

---

## Technical Notes for Jules AI
- Use `setup_conan.sh` for initial environment setup.
- Always use `build_clang.sh` for compilation. It is designed to be "blazing fast" and "token efficient" by only echoing errors to the terminal.
- Optimized scripts use gitignored directories (`build_conan/` and `build_clang/`) to ensure the workspace remains clean for integrity checks. Standard builds use `linux_build/`.
