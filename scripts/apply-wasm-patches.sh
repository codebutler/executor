#!/usr/bin/env bash
# Apply wasm submodule patches after `git submodule update --init`.
# Safe to re-run: refuses if already applied (git apply --check / --reverse).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

apply_one() {
  local sub="$1" patch="$2"
  if git -C "$sub" apply --reverse --check "$patch" 2>/dev/null; then
    echo "  already applied: $sub ($(basename "$patch"))"
    return 0
  fi
  echo "  applying: $sub ($(basename "$patch"))"
  git -C "$sub" apply "$patch"
}

echo "== applying wasm submodule patches =="
apply_one syn68k "$ROOT/patches/wasm/syn68k.patch"
apply_one PowerCore "$ROOT/patches/wasm/powercore.patch"
apply_one multiversal "$ROOT/patches/wasm/multiversal.patch"
echo "== done =="
