# FiFi OS Local AI Agent Plan (Local-First)

Goal:
A local AI helper that runs on your machine/network, does NOT send private data anywhere,
and is not controlled by external services. It can optionally search the web when you ask.

Non-negotiables:
- 100% free to keep using (no paid API requirement).
  Use local models + open-source runtimes; optional web search must work without paid keys.

- Default offline / local-only.
- No telemetry, no usage reporting.
- All logs and memory stored locally (you control deletion).
- Network access is allowlisted (off by default).
- Clear permission prompts before doing anything risky.

High-level architecture (future, after VFS + processes + networking exist):
- User-space "ai-daemon" service (NOT in the kernel).
- Shell command `ai ...` talks to the daemon via IPC.
- The daemon runs:
  - local LLM engine (ex: llama.cpp / ggml style runtime)
  - optional embeddings store for local notes (stored on disk)
  - optional web search module (only if enabled)

Security model:
- Capabilities system: the daemon gets only what you grant.
- Allowlist outbound network domains for web search.
- Separate "tools" layer for automation:
  - tools require explicit user consent
  - tools run with minimal privileges
- Safe mode: disable network + automation, keep chat only.

Milestones:
1) Shell has: `ai` command stub (now)
2) VFS + initrd file reading (done basics)
3) Processes + scheduler (later)
4) Networking stack (later)
5) ai-daemon MVP:
   - read local docs/config
   - answer questions locally
6) Optional web search:
   - user toggle + allowlist + clear prompt
7) Optional automation:
   - limited commands with confirmations
