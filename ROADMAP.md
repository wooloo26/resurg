# Resurg Language Roadmap

## v1.0 — Stable Release

Language completeness, tooling, and ecosystem.

- [ ] Language server (LSP) for editor support
- [ ] VSCode LSP extension
- [ ] Compiler diagnostics (clear error messages with source spans)

## v1.1 — Core Tooling

- [ ] Formatter (`rsg-fmt`)
- [ ] Linter (`rsg-lint` / static analysis)
- [ ] Test framework (`test` blocks or attribute)
- [ ] Unified devtool (`rsg` CLI: build/run/test/lint/fmt integration)

## Future — Post-1.1

### Core Language

- [ ] Macro / Comptime system
- [ ] Concurrency model (async/await? actors? CSP?)
- [ ] Self-hosting compiler (Resurg-in-Resurg)
- [ ] Full specification freeze

### Developer Experience

- [ ] REPL / interpreter mode
- [ ] Debugger & profiling suite (`rsg-debug`, memory tracker)

### Backends

- [ ] C++20 / Go / TypeScript backends (readable)
- [ ] LLVM IR backend
- [ ] x86-64 / ARM64 / RISC-V / WASM backends
- [ ] JVM backend (optional)

### Community

- [ ] Package manager (dependency resolution, versioning)
- [ ] Official website + documentation
- [ ] Web Playground (WASM-powered)

### GAT

- [ ] Generic Associated Types
