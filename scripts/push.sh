#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

msg="${1:-update}"

git add -A

if git diff --cached --quiet; then
  echo "Nothing to commit."
else
  git commit -m "$msg"
fi

git push
