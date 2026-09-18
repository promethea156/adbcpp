<!--
Thanks for the pull request. Please fill in the sections below and delete any
that do not apply. Target the `development` branch, not `main`.
-->

## What does this change?

<!-- A short summary, and the reason for it. Link the issue it closes, if any. -->

Closes #

## How was it verified?

<!--
Say what you ran and what happened. If the device test was not run, say why.
A green run pasted below is ideal.
-->

- [ ] `ctest --test-dir build -C Release -E "^device$" --output-on-failure` passes
- [ ] `cmake --build build --target format-check` is clean
- [ ] The device test was run, or why it was not is stated
- [ ] New behaviour has a test

## Type

<!-- Keep only the one that matches the commit type. -->

- [ ] `feat` — new feature
- [ ] `fix` — bug fix
- [ ] `docs` — documentation only
- [ ] `refactor` / `perf` — no behaviour change
- [ ] `test` — tests only
- [ ] `build` / `ci` / `chore` — tooling

## Platform

<!-- Where this was built and run. Tick what you actually verified. -->

- [ ] Windows
- [ ] Linux
- [ ] macOS
- [ ] Other:

## Checklist

- [ ] Commits follow [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/#specification)
- [ ] The branch is `<type>/<slug>` and targets `development`
- [ ] Nothing in the library throws; new fallible APIs return `Result<T>`
- [ ] Public declarations have Doxygen comments
- [ ] Docs are updated if a command, an API, or a convention changed
