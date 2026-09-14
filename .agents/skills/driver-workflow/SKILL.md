---
name: driver-workflow
description: Run the openvela NuttX driver workflow for new drivers, driver improvements, Mode C code review, or driver tests. Use when the user requests driver-workflow or a structured NuttX driver task; not for ordinary application code.
---

# Driver workflow for Codex

This repository's source workflow is `../../.claude/agents/driver-workflow.agent.md` from the openvela workspace root. Read that file before using the workflow. If invoked from a nested contest repository, locate the workspace root that contains `.claude/agents/driver-workflow.agent.md` and read it there. Its supporting skills are in the same root's `.claude/skills/` directory.

Choose the mode from the user's request: A new driver, B improvement, C review, D tests. Follow the relevant mode only. For Mode C, read `.claude/skills/driver-code-reviewer/SKILL.md`, review without changing code, then make fixes only if the user asks for them. Recheck each reported issue against the complete function and callers before changing it; record rejected false positives. For Modes A/B, use the workflow's requirements and design checkpoints, then implement and validate. Keep the original workflow's requirement to use Chinese for discussion and English for code comments and commit messages.

The `.claude` workflow is maintained separately. This Codex skill is only its discoverable entrypoint; do not copy or fork the full workflow into this directory.
