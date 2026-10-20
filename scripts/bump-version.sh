#!/usr/bin/env bash
# scripts/bump-version.sh NEW — sets the library version everywhere it is *the current version* (and nowhere
# else: historical "since 2.0.x" notes, changelog entries and test names keep their numbers). 2.0.7: written
# after two blanket sed bumps rewrote historical references.
#   scripts/bump-version.sh 2.0.8      then add the CHANGELOG.md entry and run scripts/run-all-tests.sh
set -eu
NEW=${1:?usage: scripts/bump-version.sh X.Y.Z}
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
OLD=$(sed -n 's/^project(serreg VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)
[ -n "$OLD" ] || { echo "cannot read the current version from CMakeLists.txt" >&2; exit 1; }
[ "$OLD" != "$NEW" ] || { echo "already $NEW"; exit 0; }
o=${OLD//./\\.}
sub() { # sub FILE SED-EXPRESSION — fails loudly when the expression changed nothing
    local before; before=$(md5sum "$1")
    sed -i "$2" "$1"
    [ "$before" != "$(md5sum "$1")" ] || { echo "bump-version: no change in $1 for: $2" >&2; exit 1; }
}
sub CMakeLists.txt "s/^project(serreg VERSION $o /project(serreg VERSION $NEW /"
sub include/es/serreg/version.hpp "s/library_version = \"$o\"/library_version = \"$NEW\"/"
sub python/serreg/__init__.py "s/__version__ = \"$o\"/__version__ = \"$NEW\"/"
sub python/pyproject.toml "s/^version = \"$o\"/version = \"$NEW\"/"
sub docs/DECISIONS.md "s/\*\*Refers to:\*\* library \*\*$o\*\*/**Refers to:** library **$NEW**/"
sub vcpkg.json "s/\"version\": \"$o\"/\"version\": \"$NEW\"/"
sub ports/serreg/vcpkg.json "s/\"version\": \"$o\"/\"version\": \"$NEW\"/"
sub tests/unit/test_tools.cpp "s/library_version == \"$o\"/library_version == \"$NEW\"/"
sub tests/unit/runtime_selftest.cpp "s/\"srdump $o (format/\"srdump $NEW (format/"
sub README.md "s/# srdump $o (format/# srdump $NEW (format/"
# 2.1.12 (review B-18): the tag the README's FetchContent example and the port's release comment name
sub README.md "s/GIT_TAG v$o)/GIT_TAG v$NEW)/"
sub ports/serreg/portfile.cmake "s/REF v$o SHA512/REF v$NEW SHA512/"
sub book/executables.md "s/\`srdump $o (format/\`srdump $NEW (format/"
sub book/README.md "s/^Documentation of ser-reg \*\*$o\*\*/Documentation of ser-reg **$NEW**/"
sub book/01-introduction-and-terminology.md "s/^Library version $o writes/Library version $NEW writes/; s/The book documents $o\./The book documents $NEW./"
sub book/package-contents.md "s/ser-reg-project-v$o/ser-reg-project-v$NEW/g; s/library version \`$o\`/library version \`$NEW\`/; s/srdump $o (format/srdump $NEW (format/; s/libserreg\.so\.$o/libserreg.so.$NEW/; s/libserreg_tools\.so\.$o/libserreg_tools.so.$NEW/"
sub book/files/build-system.md "s/project(serreg VERSION $o /project(serreg VERSION $NEW /; s/\`VERSION\` = $o/\`VERSION\` = $NEW/"
sub book/09-shared-library-and-loader.md "s/^\`libserreg\.so\.$o\`, SONAME/\`libserreg.so.$NEW\`, SONAME/"
sub book/files/include-es-serreg-version.hpp.md "s/library_version = \"$o\";/library_version = \"$NEW\";/"
# 2.2.4 (D2): the "current library" headers of the flattened documents and the top-level files
sub docs/ser-reg-architecture.md "s/\*\*Library:\*\* $o /**Library:** $NEW /"
sub docs/ser-reg-architecture.md "s/project(serreg VERSION $o)/project(serreg VERSION $NEW)/"
sub docs/ser-reg-architecture.md "s/\`VERSION $o\`, \`SOVERSION/\`VERSION $NEW\`, \`SOVERSION/"
sub docs/ser-reg-requirements.md "s/\*\*Library:\*\* $o /**Library:** $NEW /"
sub docs/ser-reg-requirements.md "s/as they stand at $o\*\*/as they stand at $NEW**/"
sub docs/ser-reg-stories.md "s/\*\*Library:\*\* $o /**Library:** $NEW /"
sub docs/ser-reg-testing.md "s/^Library $o, wire format/Library $NEW, wire format/"
sub docs/deviations.md "s/^# Deviations from \`ser-reg-requirements-v3.md\` — $o/# Deviations from \`ser-reg-requirements-v3.md\` — $NEW/"
sub README.md "s/^$o — wire format/$NEW — wire format/"
sub CLAUDE.md "s/\*\*Library $o, wire format/**Library $NEW, wire format/"
sub docs/serreg-reflection.md "s/^\*Library $o\. /*Library $NEW. /"
sub docs/serreg-registry.md "s/^\*Library $o\. /*Library $NEW. /"
echo "version $OLD -> $NEW; now add the '## v$NEW ' entry to CHANGELOG.md, then regenerate the golden fixtures"
echo "(the writer-info frame carries the library version: make_fixtures, make_dynamic_fixtures, runtime_selftest)"
