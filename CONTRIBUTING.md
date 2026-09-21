# Contributing to adbcpp

Thanks for helping. `adbcpp` is a small project, and the most valuable contributions are
often the ones that need no new code at all: confirming that it works on your machine,
reporting where it does not, and sharpening the documents.

There is no CLA and no sign-up. Open an issue or a pull request and say what you found.

## Ways to help

### Verify a platform

**Windows is the only platform this project has been built and run on.** Linux and macOS
are expected to work but are [marked unverified](README.md#platform-support). If you have
either, build the project, run the suite, and
[open a verification report](https://github.com/promethea156/adbcpp/issues/new?template=platform_verification.yml)
with the result, green or red. This is the single most useful thing you can do, and it needs no
knowledge of the code.

The tests that need a device skip themselves, so the suite runs on any machine:

```sh
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -E "^device$" --output-on-failure
```

`-E "^device$"` is anchored on purpose: an unanchored `-E device` also skips every test
whose name merely contains `device`, and the run still reports success. Drop the flag to run
the device test too (see [The device test](#the-device-test)).

If the build or a test fails, a report with the exact compiler, CMake version, OS, and
output is already a contribution — [open a bug](https://github.com/promethea156/adbcpp/issues/new?template=bug_report.yml).

### Pick up a roadmap item

[`docs/03-roadmap.md`](docs/03-roadmap.md) is the plan. Its
[Future Improvements](docs/03-roadmap.md#future-improvements) and
[Open Questions](docs/03-roadmap.md#open-questions) sections name the work that remains,
each linked to its issue. Items labelled
[`good first issue`](https://github.com/promethea156/adbcpp/labels/good%20first%20issue)
need little context; those labelled
[`help wanted`](https://github.com/promethea156/adbcpp/labels/help%20wanted) are broader.
Reasonable starting points today:

- **Verify the build on Linux or macOS** ([#26](https://github.com/promethea156/adbcpp/issues/26), [#25](https://github.com/promethea156/adbcpp/issues/25)) — needs a machine, since CI only compiles.

If an issue looks stale or already done, say so in it rather than guessing.

### Improve the documentation

The [guided tour](examples/demo/main.cpp), [`LEARNING.md`](LEARNING.md), and the
[design documents](docs/) are how the library is understood. If a step was unclear, that is
a bug in the docs — please fix it or open an issue. [`docs/04-blockers.md`](docs/04-blockers.md)
exists precisely so a later reader can find a better solution than the one recorded, and
corrections there are welcome.

### Send code

Read [`docs/01-objective.md`](docs/01-objective.md) for the architecture and the
[error model](docs/07-error-model.md) before writing any. In short:

- The library never throws. Every fallible operation returns a `Result<T>`.
- Public declarations carry a Doxygen comment.
- The core adds no third-party dependency beyond `tl::expected` and, when compression is built, zstd, lz4, and brotli, all linked privately; `crypto` and `usb` are optional backends.
- Comments explain *why*, matching the surrounding code.

## Set up

You need a C++20 compiler, CMake 3.24 or newer, and Git. The build is the same on every
platform; the [README](README.md#build-it) has the toolchain notes, and
[`docs/08-platform-setup.md`](docs/08-platform-setup.md) has the per-platform compiler,
USB driver, and device steps. Every dependency is fetched by CMake. Configure once and keep
the tests running as you go:

```sh
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -E "^device$" --output-on-failure
```

Check the formatting with clang-format 19.1.1 before committing:

```sh
cmake --build build --target format-check
```

`format-check` is only defined when `clang-format` is found; set
`-DADBCPP_CLANG_FORMAT=<path>` to point at a specific binary. The same check runs in CI,
so an unformatted file fails the build there.

## The device test

`adbcpp_device_tests` is the one test that needs hardware. It exits with code 77 (a CTest
skip) when no matching device is attached, so it never fails a machine without one. It always
launches, checks, and force-stops `com.android.settings`, which loses no data, and it runs an
install/uninstall round trip only when `ADBCPP_TEST_APK` and `ADBCPP_TEST_PACKAGE` name a
disposable APK. That round trip uninstalls and reinstalls the package and loses its data, so
only point it at a package you have agreed to replace.

## Commits and branches

The project follows [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/#specification)
and a four-branch model. The full rules are in
[`docs/01-objective.md`](docs/01-objective.md#commit-messages).

- Branch from `development` and name it `<type>/<slug>`, e.g. `fix/usb-short-transfer`.
- Target `development` in the pull request. `main` is release-only.
- Use a Conventional Commit type (`feat`, `fix`, `docs`, `test`, `ci`, ...). Version bumps
  are derived from the types, so an accurate type matters.

## Before you open a pull request

- [ ] The suite passes: `ctest --test-dir build -C Release -E "^device$" --output-on-failure`
- [ ] New behaviour has a unit test, driven by the mock transport where possible
- [ ] `cmake --build build --target format-check` is clean
- [ ] Public declarations have Doxygen comments
- [ ] The device test is run, or the reason it was not is stated
- [ ] Docs are updated if a command, an API, or a convention changed

The [pull request template](.github/PULL_REQUEST_TEMPLATE.md) asks for the same, so filling
it in is the checklist.

## Reviewing

Reviews are welcome too, and a review that only asks a question is useful. A reviewer is
looking for correctness, a test for the new path, the error-model shape, and a comment where
the reason for a decision is not obvious. Be kind and assume good faith; the
[blockers document](docs/04-blockers.md) shows how many sharp edges this protocol has.

## Getting help

Open an [issue](https://github.com/promethea156/adbcpp/issues/new/choose) and ask. A
question is a valid issue.
