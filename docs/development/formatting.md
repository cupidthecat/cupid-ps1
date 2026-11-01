# Formatting C sources

The root `.clang-format` follows the prevailing project style: two spaces,
120 columns, pointers beside the type, attached function braces, and
attached braces for control statements. Include order and comment text stay
as written. Existing files contain some style variations.

Use clang-format 16 or newer on the files you edit:

```sh
clang-format -i src/core/example.c src/core/example.h
```

Project C sources and headers under `src/` and `tests/` were formatted with
clang-format 21.1.5. The configuration recognizes the binary reader/writer
statement macros. Preview a file without `-i` before applying broader changes.

`dep/.clang-format` disables formatting and include sorting for vendored
code. `.clang-format-ignore` also excludes dependencies, build output and
private research in formatter versions that support ignore files.
