#!/bin/bash
# Block Bash commands that contain likely secret literals.
# Per CLAUDE.md umbrella issue #13 Task 7 (detail: #15).
set -euo pipefail

input=$(cat)
command=$(
    echo "$input" \
        | jq -r '.tool_input.command // empty'
)

if [[ -z "$command" ]]; then
    exit 0
fi

patterns=(
    # Vendor-specific API key prefixes
    'sk-[A-Za-z0-9_-]{16,}'
    'ghp_[A-Za-z0-9]{16,}'
    'gho_[A-Za-z0-9]{16,}'
    'ghu_[A-Za-z0-9]{16,}'
    'ghs_[A-Za-z0-9]{16,}'
    'github_pat_[A-Za-z0-9_]{20,}'
    'AKIA[A-Z0-9]{12,}'
    'AIza[0-9A-Za-z_-]{20,}'
    'xox[baprs]-[A-Za-z0-9-]{10,}'
    'glpat-[A-Za-z0-9_-]{20,}'
    # Generic credential assignment literals
    '[Pp]assword[[:space:]]*=[[:space:]]*[^[:space:]]+'
    '[Aa]pi[_-]?[Kk]ey[[:space:]]*=[[:space:]]*[^[:space:]]+'
    '[Aa]ccess[_-]?[Tt]oken[[:space:]]*=[[:space:]]*[^[:space:]]+'
    '[Ss]ecret[_-]?[Kk]ey[[:space:]]*=[[:space:]]*[^[:space:]]+'
    '[Aa]uth[_-]?[Tt]oken[[:space:]]*=[[:space:]]*[^[:space:]]+'
)

for pat in "${patterns[@]}"; do
    if echo "$command" | grep -Eq "$pat"; then
        cat >&2 <<ERRMSG
BLOCKED: Bash command appears to contain a secret
matching pattern: $pat
Remove the literal and use an environment variable or
credential helper instead.
ERRMSG
        exit 2
    fi
done

exit 0
