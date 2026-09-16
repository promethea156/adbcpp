# Agent Instructions

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
