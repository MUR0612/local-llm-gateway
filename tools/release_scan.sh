#!/bin/sh
# Pre-publication scan: lists every hit for manual classification.
# A hit is not automatically a problem (e.g. PROVENANCE.md must name its
# source); an empty report is not proof that publishing is allowed.
# Usage: tools/release_scan.sh [directory]
set -u
root=${1:-.}
cd "$root" || exit 2

scan() {
    label=$1
    pattern=$2
    hits=$(grep -rInE --exclude-dir=build --exclude-dir=.git --exclude-dir=private \
        --exclude=release_scan.sh -e "$pattern" . 2>/dev/null)
    count=$(printf '%s' "$hits" | grep -c . || true)
    printf '\n== %s (%s hits)\n' "$label" "$count"
    [ -n "$hits" ] && printf '%s\n' "$hits"
}

scan "student number" '\bs[0-9]{7}\b'
scan "course codes / course tooling" 'csse ?[0-9]{4}|2310(createzip|stylecheck|reformat)|gradescope|blackboard'
scan "institution names" '\bUQ\b|University of Queensland|uq\.edu|eait'
scan "campus hosts and paths" 'moss|/local/courses|svn\+ssh|source\.eait'
scan "course-specific identifiers" 'uqllm|smollm2|libcsse|A3_STATS_PORT|home\.html'
scan "personal paths" '[A-Za-z]:[\\/]Users|/home/[a-z]|/mnt/[a-z]/'
scan "email addresses" '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}'
scan "secrets" '(api[_-]?key|secret|token|passw(or)?d)[[:space:]]*[:=][[:space:]]*["'"'"']?[A-Za-z0-9_\-]{8,}'

printf '\n== files that must never be published\n'
find . -path ./build -prune -o \( -name '*.zip' -o -name '*.pdf' -o -name '.repodump' \
    -o -name 'subversion_log.txt' -o -name '.interactionlog' -o -name '.svn' \
    -o -name '.username' -o -name '.checksum' -o -name '.manifest' -o -name '.revision' \
    -o -name '.course' \) -print
exit 0
