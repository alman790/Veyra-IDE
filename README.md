<p align="center">
  <img src="resources/branding/veyra-logo.png" width="240" alt="Veyra IDE logo">
</p>

<h1 align="center">Veyra IDE</h1>

<p align="center">
  <a href="CMakeLists.txt"><img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square" alt="C++20"></a>
  <a href="#requirements"><img src="https://img.shields.io/badge/Qt-6-41CD52?style=flat-square" alt="Qt 6"></a>
  <a href=".github/workflows/ci.yml"><img src="https://img.shields.io/badge/CI-CMake%20%2B%20CTest-5865F2?style=flat-square" alt="CI: CMake and CTest"></a>
  <a href="#contributing"><img src="https://img.shields.io/badge/Contributions-welcome-22A699?style=flat-square" alt="Contributions welcome"></a>
</p>

Veyra is a desktop IDE built with C++20 and Qt 6. It connects the editor to local SDKs and language servers, builds and runs projects, and provides native debugging through LLDB. Code completion and thread review can also use a small model bundled with the application.

## Features

- **Editing:** document tabs, session recovery, split views, automatic bracket and quote pairing, completion menus, inline continuations, and Markdown preview.
- **Language tooling:** SDK discovery and selection, LSP diagnostics, semantic highlighting, completion, definitions, references, rename, hover documentation, and formatting. Availability depends on the connected language server.
- **Project workflow:** project creation, a searchable file tree, project-wide search and replace, an integrated terminal, and Git status and diff tools.
- **Build and run:** adapter-defined compiler commands and CMake project support, including target discovery through the CMake File API and Debug/Release configurations.
- **Native debugging:** LLDB breakpoints, stepping, call stacks, local variables, expression evaluation, and program input/output.
- **Thread review:** live source diagnostics and localized recommendations, with optional local model ranking.
- **Local inference:** Qwen2.5-Coder 0.5B in Q4_K_M format, approximately 491 MB, executed in-process through llama.cpp. No model service is required. Inline suggestions are accepted with Tab and dismissed with Escape.
- **Interface:** dark theme, configurable shortcuts, and English/Russian localization. A fresh installation starts in English.

Language adapters describe file types, project templates, compiler recipes, and language-server configuration. SDK discovery is read-only; connecting an SDK or installing language tooling requires an explicit action. Adapters can be imported without rebuilding the IDE.

## Requirements

| Component | Requirement |
| --- | --- |
| Compiler | C++20-capable compiler; MSVC, Clang, or GCC |
| Build system | CMake 3.20 or later |
| UI framework | Qt 6.2 or later: Core, Widgets, Concurrent; Test for the test suite |
| Model checkout | Git LFS |
| Native debugging | LLDB with its Python scripting interface |
| Language services | An SDK and a compatible language server for each language in use |

The model is stored in Git LFS. After cloning this repository, run:

```sh
git lfs install --local
git lfs pull
```

## Build and installation

Run these commands from the repository root. Build directories are excluded from Git.

### macOS

Install the Xcode Command Line Tools, then install the build dependencies:

```sh
brew install cmake qt git-lfs
git lfs install --local
git lfs pull
cmake --preset macos -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build --preset macos
ctest --preset macos
open out/macos/artifacts.noindex/VeyraIDE.app
```

For a distributable bundle and DMG/PKG installers:

```sh
cmake --build out/macos --target installer
```

The files are written to `out/macos/deployment.noindex/`. Open the DMG and drag Veyra IDE to Applications. The packaging target uses an ad-hoc signature; Developer ID signing and notarization require release credentials.

### Linux

On Ubuntu or Debian, install the build dependencies and optional C++ language/debugging tools:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake qt6-base-dev libgl1-mesa-dev libxkbcommon-dev git-lfs clangd lldb
git lfs install --local
git lfs pull
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
./out/linux/VeyraIDE
```

Install the application with `sudo cmake --install out/linux`. The `installer` target creates packages under `out/linux/packages.noindex/`; DEB and RPM generation depend on the corresponding packaging tools being installed.

### Windows

Install Visual Studio 2022 with the C++ desktop workload, CMake, Git LFS, and a Qt 6 build for MSVC 2022 x64. From a developer PowerShell, set the Qt path to the version installed on your machine:

```powershell
git lfs install --local
git lfs pull
$qt = "C:/Qt/6.8.3/msvc2022_64"
$env:PATH = "$qt/bin;$env:PATH"
cmake --preset windows -DCMAKE_PREFIX_PATH="$qt"
cmake --build --preset windows
ctest --preset windows
cmake --build out/windows --target deploy --config Release
.\out\windows\deployment.noindex\VeyraIDE.exe
```

The deployment target includes the Qt runtime. The `installer` target creates an NSIS installer when NSIS is available, along with a ZIP package.

### Build without the model

Configure with `-DTS_LOCAL_MODEL=OFF` to omit local inference and the bundled weights. SDK/LSP completion and source diagnostics remain available. A headless core build also accepts `-DTS_BUILD_GUI=OFF`; Qt Core is still required.

## First project

1. Choose **New project** or **Open project** on the welcome screen.
2. Use **Connect SDK** to inspect detected toolchains or select an SDK directory. Confirm the toolchain you want to connect.
3. Configure a language server in Settings. Where an adapter provides an installation recipe, review the command before selecting **Install and connect**.
4. Select the source file or CMake target, then build or run it. For a native executable, use F5 to start LLDB debugging.

Project configuration is stored in `.veyra/`. Custom SDK adapters may be supplied as `veyra-languages.json` or imported from Settings. The built-in adapter schema and recipes are in [resources/languages.json](resources/languages.json).

## Contributing

Open an issue for substantial changes before implementing them. For a bug report, include the operating system, compiler/Qt versions, selected SDK and language server, and steps to reproduce the problem.

Keep pull requests focused. Include a regression test when behavior changes, and describe the checks you ran. Preserve the licenses and provenance of bundled dependencies.

```sh
cmake --build <build-directory> --target check
python3 tools/format_sources.py --check
python3 tools/check_translations.py
```

The formatter requires clang-format 22 or later. Run `python3 tools/format_sources.py` to apply the repository style. Amalgamated libraries under `third_party/llama/vendor/` retain their upstream formatting.

## Continuous integration

The [CI workflow](.github/workflows/ci.yml) defines native build, test, deployment, and installer jobs for Linux, macOS, and Windows. Separate jobs cover ASan/UBSan, the Fedora core build, formatting, and translation coverage. Tool-dependent integration tests report a skip when their required backend is unavailable.

## Dependencies and scope

Third-party notices are in [licenses/](licenses/). The model's source revision and checksum are recorded in [its manifest](resources/models/manifest.json).

Refactoring support follows the connected language server. The integrated debugger currently targets native programs through LLDB.
