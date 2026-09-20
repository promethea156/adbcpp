# Agent Instructions

## Git

Never commit or push without the user's explicit approval. Do not run `git commit`, `git push`, `git tag`, or any other command that changes remote state unless the user has clearly asked for it in the current request. Preparing changes and then asking is fine.

When asking for approval, show the proposed commit message and the list of files that would be included, so the user can review exactly what will be committed.

## Branching

`main` is stable, `development` integrates feature work, `release_candidate` stabilizes a
release, and `hotfix` carries an urgent fix from `main` back to `main` and `development`. A
short-lived branch is `<type>/<slug>`. See `docs/01-objective.md` for the full model. Do not
switch the current branch or create a branch without saying so.

Work on an issue happens on a short-lived branch cut from `development`, named `<type>/<slug>`
and named after the issue (for example `feat/connection-close` for issue #11), one issue per
branch. Commit there, push it, and open a pull request into `development`; do not commit issue
work directly on `development`. The pull request is squash-merged into `development` and the branch
is deleted.

Squash-merge a short-lived branch into its target, but merge two long-lived branches with a
merge commit, not a squash; a squash between long-lived branches makes them diverge, so the target
must be merged back immediately if it happens. See `docs/01-objective.md#merging`.

## Local Tooling

### `adb` can hang the terminal

`adb` daemonizes its own server, and the server inherits the console the `adb` client was started from. When that console belongs to the agent's terminal, the server holds it open forever, so the terminal never sees EOF and the tool call hangs even after `adb` exits. Redirecting the client's output alone is not enough, because the server inherits the console, not just the output handle.

Run `adb` through [`tools/invoke-adb.ps1`](tools/invoke-adb.ps1), which starts the client in a private hidden console with its output redirected to a file at the OS level, and kills the client tree on timeout:

```powershell
. ./tools/invoke-adb.ps1
$result = Invoke-Adb -Arguments @("devices") -Adb "C:\path\to\adb.exe" -TimeoutSeconds 30
$result.Output
```

Give a command that moves a large file a correspondingly larger `-TimeoutSeconds`. `adb` also holds the device's USB interface while it runs, so stop it (`adb kill-server`) before opening the same device with `adbcpp` (blocker 5). `adb devices -l` prints each attached device's serial, which is the USB `iSerial` descriptor and the value `--serial` and `DeviceId::serial` take (blocker 30).

## Testing

Build and run the suite from the repository root:

```powershell
cmake -S . -B build -DADBCPP_BUILD_TESTS=ON -DADBCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release -E device --output-on-failure
```

Check the formatting with clang-format 19.1.1 before committing:

```powershell
cmake --build build --target format-check
```

### `-E device` also matches test names that contain `device`

`ctest -E` matches the test *name* as a regular expression, not only the test registered as `device`, so a test whose name merely contains `device` is skipped silently and the run still reports success. No test is named after the word for this reason. To exclude only the device test, anchor it:

```powershell
ctest --test-dir build -C Release -E "^device$" --output-on-failure
```

### The TCP example needs a `tcpip` device or an emulator

`adbcpp_tcp_example` connects to `localhost:5555` by default, which is an emulator's ADB listener. For a phone, run `adb tcpip 5555`, pass the phone's `host:port`, and run `adb usb` to restore USB mode. The transport is covered device-free by `tcp_test`, so CI exercises it either way.

```powershell
adb tcpip 5555
build\examples\Release\adbcpp_tcp_example.exe <phone-ip>:5555
adb usb
```

### The demo example walks every feature on one device

`adbcpp_demo_example` detects the first attached device, connects, and runs every feature once. It needs a device and an APK, and takes the package and the APK (and any split APKs) as arguments. It uninstalls the package first, so it loses that package's data.

```powershell
build\examples\Release\adbcpp_demo_example.exe <package> <apk> [<split-apk>...]
```

`am start <package>` only resolves for an app with a `MAIN`/`LAUNCHER` activity, which not every app has; the demo retries the launch for a few seconds because the package manager can still be indexing a fresh install.

### The demo_multi example runs the tour on every device at once

`adbcpp_demo_multi_example` is the same tour as `adbcpp_demo_example`, but on every attached device at once, one thread per device, so the install, launch, and uninstall steps run concurrently. It takes the package and the APK (and any split APKs), buffers each device's output, and prints the logs together at the end. Like the single-device demo, it uninstalls the package on each device first, so it loses that package's data on every device; it also uninstalls it again at the end, so it does not leave the app installed.

```powershell
build\examples\Release\adbcpp_demo_multi_example.exe <package> <apk> [<split-apk>...]
```

### The device test is destructive when it is configured to be

`adbcpp_device_tests` exits with code 77 (a CTest skip) when no matching device is attached. It always checks the install and uninstall failure paths, which touch no package, and it runs the install and uninstall round trip only when `ADBCPP_TEST_APK` and `ADBCPP_TEST_PACKAGE` name a disposable APK. That round trip uninstalls and reinstalls the package and loses its data, so only set those variables for a package the user has agreed to replace. It also always launches, checks, and force-stops `com.android.settings`, which loses no data.

## Commit Messages

All commits MUST follow the [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/#specification) specification.

Format:

```
<type>[optional scope]: <description>

[optional body]

[optional footer(s)]
```

Common types: `feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `build`, `ci`, `chore`, `style`.

- Use a scope in parentheses when useful, e.g. `feat(sync): ...`.
- Mark breaking changes with `!` after the type/scope and/or a `BREAKING CHANGE:` footer.
- Keep the description a short summary.

Examples:

```
docs: add vertical-slice implementation roadmap
feat(sync): implement push command
fix(usb): handle short USB transfers
feat(api)!: rename connect to open
```

See `docs/01-objective.md` for the full versioning and commit message standards.
