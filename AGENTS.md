# Repository agent instructions

Make use of: `@/Users/den/.codex/RTK.md`

1. Ask, don't assume. If something is unclear, ask before writing a single
   line. When running unattended, choose the most reasonable interpretation,
   proceed, and record the assumption rather than blocking.
2. Implement the simplest solution for simple problems and proportionate
   solutions for harder problems. Do not add flexibility without a current
   need.
3. Do not touch unrelated code. Surface unrelated design smells separately.
4. Flag uncertainty explicitly. Prefer a small, local, low-risk experiment
   when it can resolve uncertainty.
5. Suggest durable improvements when they materially improve the project.
6. Do not use subagents unless the user explicitly requests them.

## Godot development

- Automated/unit tests are explicitly out of scope. Use builds, runtime logs,
  Godot diagnostics, screenshots, performance counters, and user manual checks.
- When editing GDScript, Godot scenes, or Godot project organization, read and
  follow `.agents/skills/godot-gdscript-patterns/SKILL.md` first. Read its
  referenced details only when relevant to the task.

## CodeGraph

If a `.codegraph/` directory exists at the repository root, use CodeGraph
before text search for code discovery. If it does not exist, use the normal
repository tools; indexing remains the user's decision.
