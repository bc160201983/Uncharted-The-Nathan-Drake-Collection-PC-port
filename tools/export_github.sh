#!/usr/bin/env bash
# Copies the port's source into a separate Git repository (default: ../uncharted-github) and commits
# it, for the private GitHub repository that builds the Windows package (GitHub Actions).
#   gow3_pc/   the runtime: its tracked and new files (not out/, user/ or other ignored files); the
#              submodules are pinned as gitlinks at the commits checked out here
#   tools/ launcher/ Start-Uncharted.cmd analysis/STATUS.md
# Nothing from the game or a player is copied: no game/, *.pkg, analysis/ binaries, dumps, saves,
# logs or shader caches; a final check refuses the commit if such a file slipped in.
# usage: tools/export_github.sh [destination] [commit message]
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
src=$PWD
dest=${1:-../uncharted-github}
message=${2:-"Sync from the working tree"}
mkdir -p "$dest"
dest=$(cd -- "$dest" && pwd)
[[ -d $dest/.git ]] || git -C "$dest" init -q -b main

# Everything but .git is rebuilt from the working tree, so deleted files disappear from the export.
find "$dest" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +

copy() {  # copy <relative path>
    mkdir -p "$dest/$(dirname -- "$1")"
    cp -p -- "$src/$1" "$dest/$1"
}

submodules=$(git -C gow3_pc config -f .gitmodules --get-regexp '\.path$' | awk '{print $2}')
# Material of the runtime's original game stays out (its docs, launcher, patches, packaging, run
# scripts and helpers); tools/github-overlay replaces its READMEs with this project's.
git -C gow3_pc ls-files -co --exclude-standard | while read -r path; do
    case $path in
        .github/*|.gitmodules|README.md|README.*.md|docs/*|launcher/*|patches/*|run.py|run.sh) continue ;;
        packaging/linux/*|packaging/appimage.sh|packaging/default.nix|packaging/gow3-entry.c) continue ;;
        packaging/windows/README*|packaging/windows/package.sh|packaging/windows/build_dlss.sh) continue ;;
        packaging/windows/make_icon.py|tests/test_launcher_*|tests/test_button_icons.py) continue ;;
        scripts/steam_shortcut.py|scripts/save_backup.py|tools/gow3_frame_map.py) continue ;;
        tests/test_steam_shortcut.py|tests/test_mod_conflicts.py|tests/test_patches.py) continue ;;
    esac
    grep -qxF -- "$path" <<< "$submodules" && continue
    [[ -f gow3_pc/$path ]] && copy "gow3_pc/$path"
done
find tools launcher -type f ! -path '*/__pycache__/*' ! -name 'PkgTool-*.zip' \
    ! -name '*.pyc' | while read -r path; do copy "$path"; done
copy Start-Uncharted.cmd
copy analysis/STATUS.md
copy analysis/PLAN.md
for doc in analysis/MULTIPLAYER-*.md; do copy "$doc"; done   # multiplayer notes (PR #231 by Sparks)
find u3mp -type f ! -path '*/__pycache__/*' | while read -r path; do copy "$path"; done   # U3 multiplayer research (PR #244 by fidalgojoao)
cp -r tools/github/. "$dest/"
cp -r tools/github-overlay/. "$dest/"

# Submodules: .gitmodules from the runtime's, under gow3_pc/, pinned at the checked-out commits.
: > "$dest/.gitmodules"
while read -r path; do
    url=$(git -C gow3_pc config -f .gitmodules --get "submodule.$path.url")
    printf '[submodule "gow3_pc/%s"]\n\tpath = gow3_pc/%s\n\turl = %s\n' "$path" "$path" "$url" \
        >> "$dest/.gitmodules"
    mkdir -p "$dest/gow3_pc/$path"
done <<< "$submodules"

forbidden=$(cd "$dest" && find . -path ./.git -prune -o \( -iname eboot.bin -o -iname '*.elf' \
    -o -iname sce_sys -o -iname savedata -o -iname '*.log' -o -iname 'probe*.ini' -o -iname '*.pkg' \
    -o -iname '*.psarc' -o -iname '*.pak' -o -iname 'boot*.bin' -o -iname '*.spv' -o -iname '*.dmp' \
    -o -iname 'content.bin' -o -iname 'entry.bin' -o -iname '*.sav' \) -print)
if [[ -n $forbidden ]]; then
    echo "The export holds files that must not be pushed:" >&2
    echo "$forbidden" >&2
    exit 1
fi

cd "$dest"
git add -A
while read -r path; do
    sha=$(git -C "$src/gow3_pc/$path" rev-parse HEAD)
    git update-index --add --cacheinfo "160000,$sha,gow3_pc/$path"
done <<< "$submodules"
if git diff --cached --quiet; then
    echo "No changes to commit in $dest"
else
    git commit -q -m "$message" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
    git log --oneline -1
fi
