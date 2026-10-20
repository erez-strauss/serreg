#!/bin/sh
# Installs the repository's git hooks (idempotent).
set -e
root=$(git rev-parse --show-toplevel)
ln -sf ../../scripts/pre-commit "$root/.git/hooks/pre-commit"
chmod +x "$root/scripts/pre-commit"
echo "installed: .git/hooks/pre-commit -> scripts/pre-commit"
