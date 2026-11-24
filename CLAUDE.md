# Agent workflow

This repository uses project subagents defined in `.claude/agents/`:

- `cv-pose-theorist`: designs and analyzes algorithms (advisor only; does not edit code).
- `cpp-algorithm-engineer`: implements algorithms in C++.
- `unity-integration-helper`: builds the native plugin and C# bindings for Unity.
- `code-reviewer`: reviews all code changes and gives a binding verdict.

## Mandatory review loop

Subagents cannot call each other, so the main session enforces the review:

1. After any agent (or the main session) writes or changes code, run `code-reviewer` on those changes before committing.
2. If the verdict is `CHANGES REQUIRED`, send the numbered required changes back to the responsible author agent, then run `code-reviewer` again on the result.
3. Repeat until the verdict is `APPROVED`. Do not commit or push code that has not been approved. If the loop stalls after three rounds, stop and ask the user.
4. When branches or agents' changes conflict (git or logical conflicts), give `code-reviewer` the conflict to resolve before continuing.
