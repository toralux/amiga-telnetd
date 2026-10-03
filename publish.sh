#!/usr/bin/env bash
# Publish amiga-telnetd to github.com/toralux (requires gh CLI authenticated as toralux).
set -e
cd "$(dirname "$0")"
command -v m68k-amigaos-gcc >/dev/null && make || echo "(no local toolchain - CI will build)"
git init -q -b main 2>/dev/null || true
git add -A
git commit -qm "amiga-telnetd: minimal standalone telnet daemon for AmigaOS (68000)" || true
gh repo create toralux/amiga-telnetd --public --source . --push || git push -u origin main
echo "Pushed. Watch the build: gh run watch"
