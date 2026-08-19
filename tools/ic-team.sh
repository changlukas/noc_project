#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
work_root=${IC_WORK_ROOT:-"$HOME/work"}
team_root=${IC_TEAM_ROOT:-"$work_root/ic-design-team"}

case "$repo_root" in
  /mnt/*) echo "error: run agents from the Linux-native clone under $HOME/work" >&2; exit 1 ;;
esac

for path in "$team_root" "$work_root/references/FlooNoC" "$work_root/references/taxi"; do
  [[ -d "$path/.git" ]] || { echo "error: missing Linux-native repo: $path" >&2; exit 1; }
done

export NVM_DIR="$HOME/.nvm"
[[ -s "$NVM_DIR/nvm.sh" ]] || { echo "error: nvm is not installed" >&2; exit 1; }
. "$NVM_DIR/nvm.sh"
nvm use --silent "$(cat "$team_root/.nvmrc")"

docker info >/dev/null
docker image inspect ic-design-team:latest >/dev/null
gh auth status >/dev/null
[[ -r "$HOME/.codex/auth.json" ]] || { echo "error: missing Codex auth" >&2; exit 1; }

for path in "$repo_root" "$team_root" "$work_root/references/FlooNoC" "$work_root/references/taxi"; do
  [[ -z "$(git -C "$path" status --porcelain)" ]] || {
    echo "error: dirty repository: $path" >&2
    exit 1
  }
done

echo "preflight: Node $(node --version), Docker ready, Linux-native repositories clean"
[[ ${1:-} == "--preflight" ]] && exit 0

export FLOONOC_REFERENCE="$work_root/references/FlooNoC"
export TAXI_REFERENCE="$work_root/references/taxi"
exec npm --prefix "$team_root" run team -- \
  --repo "$repo_root" --config ic-team.config.mts "$@"
