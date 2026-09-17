# Agent Instructions

## Git

Never commit or push without the user's explicit approval. Do not run `git commit`, `git push`, `git tag`, or any other command that changes remote state unless the user has clearly asked for it in the current request. Preparing changes and then asking is fine.

When asking for approval, show the proposed commit message and the list of files that would be included, so the user can review exactly what will be committed.

## Local Tooling

### `adb` can hang the terminal

`adb` daemonizes its own server, and the server inherits the console the `adb` client was started from. When that console belongs to the agent's terminal, the server holds it open forever, so the terminal never sees EOF and the tool call hangs even after `adb` exits. Redirecting the client's output alone is not enough, because the server inherits the console, not just the output handle.

Run `adb` through [`tools/invoke-adb.ps1`](tools/invoke-adb.ps1), which starts the client in a private hidden console with its output redirected to a file at the OS level, and kills the client tree on timeout:

```powershell
. ./tools/invoke-adb.ps1
$result = Invoke-Adb -Arguments @("devices") -Adb "C:\path\to\adb.exe" -TimeoutSeconds 30
$result.Output
```

Give a command that moves a large file a correspondingly larger `-TimeoutSeconds`. `adb` also holds the device's USB interface while it runs, so stop it (`adb kill-server`) before opening the same device with `adbcpp` (blocker 5).

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
