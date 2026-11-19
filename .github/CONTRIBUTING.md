# Contributing

Use Issues for reproducible bugs and scoped feature proposals. Discuss larger changes before opening a pull request.

## Development

Install the compiler, Qt, CMake, and Git LFS dependencies listed in the README. Fetch the model with `git lfs pull`, then configure the preset for your platform.

```sh
cmake --build <build-directory> --target check
cmake --build <build-directory> --target format-check translations-check
```

The formatter used by CI is clang-format 22.1.8. Keep generated files and build outputs outside the source tree. Do not commit SDK paths, session state, binaries, or credentials.

## Pull requests

- Keep each change focused and explain the resulting behavior.
- Include a regression test for behavior changes. State which checks ran and which require an unavailable SDK.
- Update the English/Russian catalog when changing visible text.
- Preserve third-party notices and the model manifest.
- Use short, two-word, lowercase commit subjects, such as `fix completion` or `update docs`.

Application code is covered by the root MIT license. Dependencies retain their own licenses.

## Releases

Wait for a successful CI run on `main`, then create a version tag at that run's exact commit. The tag version must match `CMakeLists.txt`.

Run the **Release** workflow with the tag and CI run ID. It reuses the tested native packages, uploads SHA-256 checksums and build provenance, and prepares a draft by default. Inspect the draft before publishing it. Published releases are not overwritten by the workflow.
