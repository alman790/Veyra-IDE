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
