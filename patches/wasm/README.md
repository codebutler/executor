# Wasm submodule patches

These teach the still-autc04 submodules (`syn68k`, `PowerCore`, `multiversal`)
about `__wasm__` / node-runnable codegen / clang-format. They are **generic
wasm port glue**, not pc-specific integration.

Apply after submodule init:

```sh
git submodule update --init --depth 1 syn68k PowerCore multiversal
bash scripts/apply-wasm-patches.sh
```

When these land in forked submodules (or upstream), delete this directory.
