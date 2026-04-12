# rsg-lsp

Language server for Resurg, implementing the [Language Server Protocol](https://microsoft.github.io/language-server-protocol/) over stdin/stdout.

## Features

- **Diagnostics** — Real-time error reporting with source spans as you type
- **Full document sync** — Lex, parse, and type-check on every change

## Usage

```sh
rsg-lsp [--std-path=DIR]
```

The server communicates via JSON-RPC over stdin/stdout using `Content-Length` framing.

## VSCode Extension

See [editors/vscode/](editors/vscode/) for the VSCode extension that connects to this server.

## Build

Built automatically with `make all`. The binary is placed at `build/rsg-lsp`.
