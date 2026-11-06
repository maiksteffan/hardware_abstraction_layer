#!/usr/bin/env bash
# @file release.sh
# @brief Prepare and push a version-matched firmware release for GitHub Actions.
set -euo pipefail

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
usage() {
    cat <<'EOF'
Usage: ./release.sh [--dry-run] [--yes] [--version MAJOR.MINOR.PATCH]

Default: increment the patch version, ask for confirmation, run native tests
and both ESP32 builds, update release metadata, commit, tag and push atomically.
Commit your firmware changes and this script first: real releases require a
clean working tree. The current branch is released; master is not substituted.

  --dry-run       Show the plan only. No writes, builds, commits, tags or network.
  --yes           Skip confirmation (all safety checks still apply).
  --version X.Y.Z Choose a strictly newer version instead of a patch increment.
  --help          Show this help.

GitHub Actions publishes the binaries AFTER its build succeeds. This script
does not flash hardware or publish an empty release. Do not create one manually.
Requirements: git, perl, PlatformIO (PATH or ~/.platformio/penv/bin/pio).
EOF
}

dry_run=false
assume_yes=false
version=''
while (( $# )); do
    case "$1" in
        --dry-run) dry_run=true; shift ;;
        --yes) assume_yes=true; shift ;;
        --version)
            (( $# >= 2 )) || die '--version needs a value.'
            version="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) die "Unknown argument: $1 (use --help)." ;;
    esac
done

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$root"
command -v git >/dev/null || die 'git is required.'
command -v perl >/dev/null || die 'perl is required.'
repo_root="$(git rev-parse --show-toplevel)"
[[ "$root" == "$(cd "$repo_root" && pwd -P)" ]] || die 'Put release.sh in the repository root.'
branch="$(git symbolic-ref --quiet --short HEAD)" || die 'Detached HEAD: switch to the branch you want to release.'
for file in include/Config.h README.md claude.md .github/workflows/release.yml; do
    [[ -f "$file" ]] || die "Missing required file: $file"
done

current="$(perl -ne 'print "$1\n" if /^#define\s+FIRMWARE_VERSION\s+"([^"]+)"/' include/Config.h)"
# Bound components for portable shell arithmetic and reject leading zeros.
semver='^(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})$'
[[ "$current" =~ $semver ]] || die "Invalid or duplicated FIRMWARE_VERSION: $current"
IFS=. read -r major minor patch <<< "$current"
[[ -n "$version" ]] || version="$major.$minor.$((patch + 1))"
[[ "$version" =~ $semver ]] || die "Invalid version: $version"
IFS=. read -r next_major next_minor next_patch <<< "$version"
(( next_major > major || (next_major == major && next_minor > minor) ||
   (next_major == major && next_minor == minor && next_patch > patch) )) || die 'The release version must be newer than the current firmware.'
for tag in "$version" "v$version"; do
    if git show-ref --verify --quiet "refs/tags/$tag"; then
        die "Tag $tag already exists. Never reuse a released tag."
    fi
done
remote="$(git remote get-url --push origin)" || die 'Configure the origin remote first.'
dirty="$(git status --porcelain --untracked-files=normal)"
pio=''
if command -v pio >/dev/null; then
    pio="$(command -v pio)"
elif [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
    pio="$HOME/.platformio/penv/bin/pio"
fi

printf 'Branch:   %s\nRemote:   %s\nFirmware: %s -> %s\nTag:      %s\n' "$branch" "$remote" "$current" "$version" "$version"
printf '\nPlan:\n  1. Update Config.h, README.md and claude.md.\n  2. Run native tests and both ESP32 builds.\n  3. Commit on %s and tag that exact commit.\n  4. Atomically push that branch and tag to origin.\n  5. GitHub Actions builds and publishes the download assets.\n' "$branch"
if $dry_run; then
    [[ -z "$dirty" ]] || printf '\nNOTE: Commit these changes before a real release:\n%s\n' "$dirty"
    [[ -n "$pio" ]] || printf '\nNOTE: Install PlatformIO before a real release.\n'
    printf '\nDRY RUN: nothing changed; remote tag collisions were not checked.\n'
    exit 0
fi

[[ -z "$dirty" ]] || die 'Working tree is not clean. Commit your intended changes (including release.sh), then rerun. Nothing has been changed.'
[[ -n "$pio" ]] || die 'PlatformIO was not found on PATH or in ~/.platformio/penv/bin/pio.'
git var GIT_AUTHOR_IDENT >/dev/null
git var GIT_COMMITTER_IDENT >/dev/null
git ls-files --error-unmatch include/Config.h README.md claude.md .github/workflows/release.yml release.sh >/dev/null
remote_tags="$(git ls-remote --tags origin "refs/tags/$version" "refs/tags/v$version")"
[[ -z "$remote_tags" ]] || die "Version $version already exists on origin. Nothing has been changed."
if ! $assume_yes; then
    printf '\nRelease %s from %s? Type yes: ' "$version" "$branch"
    read -r answer || die 'No confirmation received.'
    [[ "$answer" == yes ]] || die 'Cancelled. Nothing has been changed.'
fi

backup="$(mktemp -d "${TMPDIR:-/tmp}/sequenzboard-release.XXXXXX")"
cp include/Config.h "$backup/Config.h"
cp README.md "$backup/README.md"
cp claude.md "$backup/claude.md"
metadata_updated=false
committed=false
tagged=false
cleanup() {
    result=$?
    trap - EXIT
    if (( result != 0 )); then
        if $metadata_updated && ! $committed; then
            cp "$backup/Config.h" include/Config.h
            cp "$backup/README.md" README.md
            cp "$backup/claude.md" claude.md
            git restore --staged -- include/Config.h README.md claude.md || true
            printf '\nRelease failed before commit; original release metadata restored. Nothing was pushed.\n' >&2
        elif $committed; then
            printf '\nRelease commit kept locally. Check GitHub before retrying; do not create another version just to retry this push.\n' >&2
            if $tagged; then
                printf 'Retry: git push --atomic origin %q %q\n' "HEAD:refs/heads/$branch" "refs/tags/$version:refs/tags/$version" >&2
            fi
        fi
    fi
    rm -rf "$backup"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Edit only release metadata, never historical version/changelog entries.
metadata_updated=true
OLD_VERSION="$current" NEW_VERSION="$version" perl -0777 -i -pe '
    BEGIN { $old = $ENV{OLD_VERSION}; $new = $ENV{NEW_VERSION}; }
    if ($ARGV eq "include/Config.h") {
        s/^(#define\s+FIRMWARE_VERSION\s+")\Q$old\E(")/${1}$new${2}/m == 1
            or die "Expected exactly one firmware version\n";
    } elsif ($ARGV eq "README.md") {
        s/^(Current firmware: \*\*)\Q$old\E(\*\*\.)$/${1}$new${2}/m == 1
            or die "README current-version marker missing or out of sync\n";
    } elsif ($ARGV eq "claude.md") {
        s/^(\| `FIRMWARE_VERSION` \| `)[^`\r\n]+(` \|)$/${1}$new${2}/m == 1
            or die "claude.md firmware-version row missing\n";
        s/INFO firmware=\Q$old\E(?= protocol=)/INFO firmware=$new/g;
        s/(# 8\. Changelog.*?\|---\|---\|\n)/$1| $new | Automated release; see Git history and GitHub release notes for changes. |\n/s == 1
            or die "claude.md changelog table missing\n";
    }
' include/Config.h README.md claude.md

"$pio" test -e native
"$pio" run -e esp32-s3-devkitc-1 -e esp32dev
git diff --check
git add -- include/Config.h README.md claude.md
git commit -m "Release firmware $version"
committed=true
committed_version="$(git show HEAD:include/Config.h | perl -ne 'print "$1\n" if /^#define\s+FIRMWARE_VERSION\s+"([^"]+)"/')"
[[ "$committed_version" == "$version" ]] || die 'Committed firmware version differs from planned tag; refusing to tag.'
[[ "$(git symbolic-ref --quiet --short HEAD)" == "$branch" ]] || die 'Branch changed during release; refusing to tag.'
[[ -z "$(git status --porcelain --untracked-files=normal)" ]] || die 'Working tree changed during validation; refusing to tag.'
git tag -a "$version" -m "Firmware $version"
tagged=true
# No force push. If either ref is rejected, neither ref is updated remotely.
git push --atomic origin "HEAD:refs/heads/$branch" "refs/tags/$version:refs/tags/$version"
printf '\nPushed release %s. Wait for Build & Release Firmware to succeed on GitHub Actions, then check for updates on the Pi.\n' "$version"
printf 'No hardware was flashed and no empty GitHub release was created locally.\n'