#!/usr/bin/env bash
# @file release_script_test.sh
# @brief Test release automation in disposable repositories with mocked builds.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/sequenzboard-release-tests.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_TERMINAL_PROMPT=0
mkdir "$tmp/bin"
cat > "$tmp/bin/pio" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$MOCK_PIO_LOG"
exit "${MOCK_PIO_EXIT:-0}"
EOF
chmod +x "$tmp/bin/pio"
export PATH="$tmp/bin:$PATH"
export MOCK_PIO_LOG="$tmp/pio.log"
cases=0

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { cases=$((cases + 1)); printf 'PASS: %s\n' "$*"; }
assert_contains() { grep -Fq -- "$2" "$1" || fail "$1 does not contain $2"; }
new_fixture() {
    repo="$tmp/case-$cases"
    remote="$tmp/remote-$cases.git"
    mkdir -p "$repo/include" "$repo/.github/workflows"
    git init -q --bare "$remote"
    git init -q "$repo"
    cd "$repo"
    git config user.name 'Release Test'
    git config user.email 'release-test@example.invalid'
    git config commit.gpgsign false
    git config tag.gpgsign false
    git checkout -qb feature/release-test
    cp "$root/release.sh" release.sh
    cp "$root/.github/workflows/release.yml" .github/workflows/release.yml
    printf '#define FIRMWARE_VERSION "1.3.4"\n' > include/Config.h
    printf 'Current firmware: **1.3.4**.\n' > README.md
    cat > claude.md <<'EOF'
| Constant | Value |
|---|---|
| `FIRMWARE_VERSION` | `1.3.4` |
INFO firmware=1.3.4 protocol=3
# 8. Changelog
| Version | Change |
|---|---|
| 1.3.0 | Historical holdgauge release |
EOF
    git add .
    git commit -qm 'Fixture firmware'
    git remote add origin "$remote"
    git push -q origin HEAD:refs/heads/feature/release-test
    original="$(git rev-parse HEAD)"
    export MOCK_PIO_EXIT=0
    : > "$MOCK_PIO_LOG"
}
expect_failure() {
    if bash release.sh "$@" > "$tmp/output" 2>&1; then
        fail "Expected failure for arguments: $*"
    fi
}
assert_no_release() {
    [[ "$(git rev-parse HEAD)" == "$original" ]] || fail 'Unexpected commit'
    [[ -z "$(git tag -l)" ]] || fail 'Unexpected tag'
    [[ "$(git --git-dir="$remote" rev-parse refs/heads/feature/release-test)" == "$original" ]] || fail 'Unexpected remote change'
}

new_fixture
bash release.sh --dry-run > "$tmp/output"
assert_contains "$tmp/output" 'Firmware: 1.3.4 -> 1.3.5'
assert_no_release
[[ -z "$(git status --porcelain)" && ! -s "$MOCK_PIO_LOG" ]] || fail 'Dry run changed files or built'
pass 'dry run has no side effects'

new_fixture
printf '\nLocal edit\n' >> README.md
bash release.sh --dry-run > "$tmp/output"
assert_contains "$tmp/output" 'Commit these changes before a real release'
expect_failure --yes
assert_contains "$tmp/output" 'Working tree is not clean'
assert_no_release
assert_contains README.md 'Local edit'
pass 'dirty trees can be previewed but cannot be released'

new_fixture
if printf 'no\n' | bash release.sh > "$tmp/output" 2>&1; then
    fail 'Expected cancellation'
fi
assert_contains "$tmp/output" 'Cancelled. Nothing has been changed.'
assert_no_release
[[ -z "$(git status --porcelain)" && ! -s "$MOCK_PIO_LOG" ]] || fail 'Cancelled release edited or built'
pass 'confirmation can be cancelled without side effects'

new_fixture
git remote set-url origin "$tmp/nonexistent-remote"
bash release.sh --dry-run > "$tmp/output"
expect_failure --yes
assert_no_release
[[ -z "$(git status --porcelain)" ]] || fail 'Unreachable remote edited metadata'
pass 'dry runs are offline and unreachable remotes fail before editing'

new_fixture
expect_failure --version 1.3.4 --yes
assert_no_release
expect_failure --version 01.3.5 --yes
assert_no_release
expect_failure --version invalid --yes
assert_no_release
pass 'invalid and non-increasing versions are rejected'

new_fixture
git tag v1.3.5
expect_failure --dry-run
assert_contains "$tmp/output" 'Tag v1.3.5 already exists'
[[ "$(git rev-parse HEAD)" == "$original" ]] || fail 'Tag collision created commit'
pass 'local tags with either naming convention are protected'

new_fixture
git tag v1.3.5
git push -q origin refs/tags/v1.3.5
git tag -d v1.3.5 >/dev/null
expect_failure --yes
assert_contains "$tmp/output" 'already exists on origin'
assert_no_release
pass 'remote tag collisions are caught before editing files'

new_fixture
export MOCK_PIO_EXIT=1
expect_failure --yes
assert_contains "$tmp/output" 'original release metadata restored'
assert_no_release
[[ -z "$(git status --porcelain)" ]] || fail 'Failed build did not restore metadata'
pass 'failed validation rolls back metadata without publishing'

new_fixture
cat > .git/hooks/pre-commit <<'EOF'
#!/usr/bin/env bash
exit 1
EOF
chmod +x .git/hooks/pre-commit
expect_failure --yes
assert_no_release
[[ -z "$(git status --porcelain)" ]] || fail 'Failed commit left staged metadata'
pass 'failed commits restore both metadata and staging'

new_fixture
printf 'Missing version marker\n' > README.md
git add README.md
git commit -qm 'Bad documentation fixture'
original="$(git rev-parse HEAD)"
git push -q origin HEAD:refs/heads/feature/release-test
expect_failure --yes
assert_no_release
[[ -z "$(git status --porcelain)" ]] || fail 'Metadata error did not roll back'
pass 'missing documentation markers fail safely'

new_fixture
git checkout -q --detach
expect_failure --dry-run
assert_contains "$tmp/output" 'Detached HEAD'
assert_no_release
pass 'detached HEAD cannot be released'

new_fixture
bash release.sh --yes > "$tmp/output" 2>&1
assert_contains include/Config.h '#define FIRMWARE_VERSION "1.3.5"'
assert_contains README.md 'Current firmware: **1.3.5**.'
assert_contains claude.md '| `FIRMWARE_VERSION` | `1.3.5` |'
assert_contains claude.md 'INFO firmware=1.3.5 protocol=3'
assert_contains claude.md '| 1.3.0 | Historical holdgauge release |'
assert_contains claude.md '| 1.3.5 | Automated release;'
[[ "$(git rev-parse HEAD)" == "$(git rev-parse '1.3.5^{commit}')" ]] || fail 'Tag does not match release commit'
[[ "$(git --git-dir="$remote" rev-parse refs/heads/feature/release-test)" == "$(git rev-parse HEAD)" ]] || fail 'Wrong branch was pushed'
[[ "$(git --git-dir="$remote" rev-parse 'refs/tags/1.3.5^{commit}')" == "$(git rev-parse HEAD)" ]] || fail 'Remote tag differs from commit'
assert_contains "$MOCK_PIO_LOG" 'test -e native'
assert_contains "$MOCK_PIO_LOG" 'run -e esp32-s3-devkitc-1 -e esp32dev'
[[ -z "$(git status --porcelain)" ]] || fail 'Successful release left dirty metadata'
pass 'successful release updates docs, validates and tags the exact feature commit'

new_fixture
bash release.sh --version 1.4.0 --yes > "$tmp/output" 2>&1
assert_contains include/Config.h '#define FIRMWARE_VERSION "1.4.0"'
[[ "$(git rev-parse '1.4.0^{commit}')" == "$(git rev-parse HEAD)" ]] || fail 'Explicit version tag differs'
pass 'explicit newer versions are supported'

new_fixture
cat > "$remote/hooks/pre-receive" <<'EOF'
#!/usr/bin/env bash
while read -r old new ref; do
    if [[ "$ref" == refs/tags/* ]]; then
        echo 'Fixture refuses tags' >&2
        exit 1
    fi
done
EOF
chmod +x "$remote/hooks/pre-receive"
expect_failure --yes
assert_contains "$tmp/output" 'Retry: git push --atomic origin'
[[ "$(git rev-parse HEAD)" != "$original" ]] || fail 'Failed push discarded local commit'
[[ "$(git rev-parse '1.3.5^{commit}')" == "$(git rev-parse HEAD)" ]] || fail 'Failed push discarded tag'
[[ "$(git --git-dir="$remote" rev-parse refs/heads/feature/release-test)" == "$original" ]] || fail 'Rejected atomic push updated branch'
[[ -z "$(git --git-dir="$remote" tag -l)" ]] || fail 'Rejected atomic push uploaded tag'
pass 'rejected atomic pushes keep retryable local refs without partial publication'

printf '\n%d release-script tests passed. No real remote or hardware was used.\n' "$cases"