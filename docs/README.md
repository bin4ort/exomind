# Exomind AINSS Documentation Index

## Component Documentation

| Component | Description | Documentation |
|-----------|-------------|---------------|
| **exomind** | Durable long-term memory (KV, notes, vectors, search) | [components/exomind.md](components/exomind.md) |
| **exosched** | Alarm clock for AI agents (timers, reminders, WS push) | [components/exosched.md](components/exosched.md) |
| **exoflow** | Swarm orchestrator (DAG flows, claims, deadlines) | [components/exoflow.md](components/exoflow.md) |
| **exoqms** | Quality Management System (ISO 9001/19011) | [components/exoqms.md](components/exoqms.md) |
| **exocrawl** | AI-native web research (metasearch, extraction) | [components/exocrawl.md](components/exocrawl.md) |
| **exocontext** | Context continuity (bounded digest, auto-compress) | [components/exocontext.md](components/exocontext.md) |
| **exodoc** | Documentation auditor (ISO 9001 §7.5 standard) | [components/exodoc.md](components/exodoc.md) |
| **exokit** | Behavioral development kit (contracts, examples, runners) | [components/exokit.md](components/exokit.md) |
| **exogit** | GitHub/GitLab content analyzer | [components/exogit.md](components/exogit.md) |
| **exogpl** | GPL v3.0 compliance checker | [components/exogpl.md](components/exogpl.md) |

## Kit (Behavioral Contract) Documentation

Each component carries its own `kit/` — a self-SDK with function inventory, behavioral examples, and runner shims.

| Kit | Documentation |
|-----|---------------|
| **core** (exomind) | [kits/core.md](kits/core.md) |
| **exosched** | [kits/exosched.md](kits/exosched.md) |
| **exoflow** | [kits/exoflow.md](kits/exoflow.md) |
| **exodoc** | [kits/exodoc.md](kits/exodoc.md) |
| **exocrawl** | [kits/exocrawl.md](kits/exocrawl.md) |
| **exocontext** | [kits/exocontext.md](kits/exocontext.md) |
| **exoqms** | [kits/exoqms.md](kits/exoqms.md) |

## Packaging

| Platform | Documentation |
|----------|---------------|
| **Arch Linux** (PKGBUILD) | [packaging/arch.md](packaging/arch.md) |
| **Debian/Ubuntu** | [packaging/debian.md](packaging/debian.md) |
| **Guix** | [packaging/guix.md](packaging/guix.md) |

## Stack Overview

- [STACK.md](STACK.md) — Complete stack architecture, ports, data flows
- [stack.tsv](stack.tsv) — Machine-readable component manifest

## Quick Links

- **MCP Router**: `exomind-server` — 20 tools over stdio JSON-RPC
- **Console Ops**: `exo<module> /operation?query` — one-shot, no daemon
- **HTTP Server**: `exomind --serve` — binds port 7654
- **Quality Gate**: `make test-exodoc && make audit-stack`
- **Exokit Gate**: `exokit verify --kit <dir> && exokit audit --kit <dir>`

## MCP Router Flags (New)

```
--exclude-module <mods>     Comma-separated modules to exclude (exokit,exodoc,spawn,...)
--no-auto-context           Disable automatic exocontext loading before tool calls
--no-auto-store             Disable automatic result storage in exomind after tool calls
--no-verify-claims          Disable external claim verification (GitHub/version scans)
```

Environment variable equivalents: `EXO_EXCLUDE_MODULES`, `EXO_NO_AUTO_CONTEXT`, `EXO_NO_AUTO_STORE`, `EXO_NO_VERIFY_CLAIMS`