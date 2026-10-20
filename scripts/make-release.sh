#!/usr/bin/env bash
# scripts/make-release.sh — package the working tree as a versioned release tarball.
#
#   scripts/make-release.sh [--out DIR]           → DIR/ser-reg-project-v<version>.tar.gz, top-level dir ser-reg-project-v<version>/
#   scripts/make-release.sh --public [--out DIR]  → DIR/ser-reg-public-v<version>.tar.gz, top-level dir ser-reg-public-v<version>/:
#                                                   only the files public-files-list.txt selects (2.1.11)
#
# The version is read from project(serreg VERSION x.y.z) in CMakeLists.txt. The tarball is built from an explicit
# exclude list (build trees, logs, caches, Testing/, and any capture/RRI files outside tests/fixtures — the 1.1.0 and
# 2.0.0 tarballs shipped a stray file_store.ser because an example had run in the repository root). It refuses to
# overwrite an existing tarball (release filenames are never reused), checks that CHANGELOG.md has an entry for the
# version and that the tree is clang-format clean, and prints the file count and the sha256. --public also checks
# the list: every file is classified, no file is both public and excluded, no public line is stale; references from
# public files to excluded ones are printed as warnings.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(dirname "$REPO")
PUBLIC=0
while [ $# -gt 0 ]; do
    case $1 in
    --out) OUT=$2; shift 2 ;;
    --public) PUBLIC=1; shift ;;
    -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
VERSION=$(sed -n 's/^project(serreg VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")
[ -n "$VERSION" ] || { echo "cannot read the version from CMakeLists.txt" >&2; exit 1; }
NAME=ser-reg-project-v$VERSION
[ "$PUBLIC" = 0 ] || NAME=ser-reg-public-v$VERSION
TARBALL=$OUT/$NAME.tar.gz
[ ! -e "$TARBALL" ] || { echo "$TARBALL exists: release filenames are never reused" >&2; exit 1; }
grep -q "^## v$VERSION " "$REPO/CHANGELOG.md" || { echo "CHANGELOG.md has no '## v$VERSION' entry" >&2; exit 1; }
# 2.1.12 (review B-17): the format and book checks are part of making a release, not optional extras
command -v clang-format >/dev/null || { echo "clang-format (22, the reference) not found: cannot check the format" >&2; exit 1; }
(cd "$REPO" && find include src tools tests examples -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 |
    xargs -0 clang-format --style=file --dry-run --Werror) || { echo "tree is not clang-format clean" >&2; exit 1; }
python3 "$REPO/scripts/check-book.py" --quiet || { echo "the book is out of sync with the sources" >&2; exit 1; }
# --public: keep only what public-files-list.txt selects, after checking the list against the staged tree ($1).
select_public() {
    local root=$1 list=$REPO/public-files-list.txt line pat f n_pub n_exc bad=0 refs=0
    [ -f "$list" ] || { echo "public-files-list.txt not found" >&2; exit 1; }
    local -a pub=() exc=() files=() keep=()
    while IFS= read -r line || [ -n "$line" ]; do
        case $line in
        '#private:'* | '#decide:'*) pat=${line#*:}; read -r pat _ <<<"$pat"; exc+=("$pat") ;;
        '#'* | '') ;;
        *) read -r pat _ <<<"$line"; pub+=("$pat") ;;
        esac
    done <"$list"
    # regular files and symbolic links (2026-10-01: ce-examples/ links its headers into include/; `-type f` dropped them)
    mapfile -t files < <(cd "$root" && find . \( -type f -o -type l \) | sed 's|^\./||' | LC_ALL=C sort)
    matches() { # file pattern
        case $2 in */) [[ $1 == "$2"* ]] ;; *) [[ $1 == $2 ]] ;; esac
    }
    local -A used=()
    for f in "${files[@]}"; do
        n_pub=0 n_exc=0
        for pat in "${pub[@]}"; do matches "$f" "$pat" && { n_pub=$((n_pub + 1)); used[$pat]=1; }; done
        for pat in "${exc[@]}"; do matches "$f" "$pat" && n_exc=$((n_exc + 1)); done
        if [ $((n_pub + n_exc)) = 0 ]; then
            echo "public-files-list.txt: unclassified file: $f" >&2; bad=1
        elif [ "$n_pub" -gt 0 ] && [ "$n_exc" -gt 0 ]; then
            echo "public-files-list.txt: both public and excluded: $f" >&2; bad=1
        elif [ "$n_pub" -gt 0 ]; then
            keep+=("$f")
        fi
    done
    for pat in "${pub[@]}"; do
        [ -n "${used[$pat]:-}" ] || { echo "public-files-list.txt: public line matches no file: $pat" >&2; bad=1; }
    done
    [ "$bad" = 0 ] || exit 1
    # references from public text files to excluded ones (warnings: the public tree would cite a missing file)
    for pat in "${exc[@]}"; do
        local needle=${pat%\*}
        case $pat in */) needle=$(basename "$pat")/ ;; *) needle=$(basename "$needle") ;; esac
        local hits
        hits=$(cd "$root" && grep -F -l -- "$needle" "${keep[@]}" 2>/dev/null | tr '\n' ' ' || true)
        [ -z "$hits" ] || { echo "warning: public files cite excluded '$needle': $hits" >&2; refs=$((refs + 1)); }
    done
    local pubroot
    pubroot=$(mktemp -d)
    (cd "$root" && printf '%s\0' "${keep[@]}" | tar -cf - --null -T -) | (cd "$pubroot" && tar -xf -)
    # a public symbolic link must point at a public file (or directory): no dangling link in the public tree
    local dangling
    dangling=$(cd "$pubroot" && find . -xtype l | sed 's|^\./||' | head -5)
    [ -z "$dangling" ] || { echo "public symbolic links to unpublished targets: $dangling" >&2; rm -rf "$pubroot"; exit 1; }
    chmod 755 "$pubroot" && rm -rf "$root" && mv "$pubroot" "$root"
    echo "public subset: ${#keep[@]} of ${#files[@]} files; $refs excluded name(s) cited by public files"
}
# 2.1.15 (review R2): compiled Python caches are machine-specific and never part of a release (2.1.14 shipped
# scripts/__pycache__/). They are removed from the working tree, which is always safe, rather than excluded, so the
# completeness check below still sees every real file.
find "$REPO" -name __pycache__ -type d -not -path "$REPO/build*" -prune -exec rm -rf {} +
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/$NAME"
# --no-wildcards-match-slash: without it GNU tar lets '*' cross '/', and './*.ser' would also drop
# tests/fixtures/**/*.ser (a 2.0.1 pre-release tarball lost its golden fixtures exactly that way).
(cd "$REPO" && tar -cf - --anchored --no-wildcards-match-slash \
    --exclude=./build --exclude='./build-*' --exclude=./logs --exclude=./Testing --exclude='./.cache*' \
    --exclude='./.ccache*' \
    --exclude=./.git --exclude=./compile_commands.json \
    --exclude='./*.ser' --exclude='./*.rri' --exclude='./examples/*.ser' .) | (cd "$STAGE/$NAME" && tar -xf -)
stray=$(cd "$STAGE/$NAME" && find . \( -name '*.ser' -o -name '*.rri' \) -not -path './tests/fixtures/*' | head -5)
[ -z "$stray" ] || { echo "stray capture files outside tests/fixtures: $stray" >&2; exit 1; }
# completeness: every file under tests/fixtures and the other shipped trees must be in the stage
missing=$(cd "$REPO" && find tests/fixtures include src tools examples book docs scripts share cmake ports -type f |
    while read -r f; do [ -e "$STAGE/$NAME/$f" ] || echo "$f"; done | head -5)
[ -z "$missing" ] || { echo "files missing from the release stage: $missing" >&2; exit 1; }
if [ "$PUBLIC" = 1 ]; then
    select_public "$STAGE/$NAME"
fi
# 2.1.12 (review B-17): reproducible — sorted names, fixed owner, every mtime the release date from CHANGELOG.md,
# gzip without a timestamp — so the same tree gives the same bytes (and the same sha256 for the vcpkg port)
RELEASE_DATE=$(sed -n "s/^## v$VERSION — \([0-9-]*\).*/\1/p" "$REPO/CHANGELOG.md" | head -1)
(cd "$STAGE" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="${RELEASE_DATE:-1970-01-01} 00:00:00Z" \
    -cf - "$NAME" | gzip -9n > "$TARBALL")
echo "$TARBALL: $(tar -tzf "$TARBALL" | grep -vc '/$') files, sha256 $(sha256sum "$TARBALL" | cut -d' ' -f1)"
# 2.2.1 (DECISIONS D-2): the single-file rebuild prompt next to the tarball (the project tarball only; the
# public one is a subset of the same tree, so one prompt per version is enough)
if [ "$PUBLIC" != 1 ]; then
    python3 "$REPO/scripts/make-rebuild-prompt.py" --out "$OUT"
fi
