#!/usr/bin/env bash
# What the VERSION file means for a build.
#
# VERSION is either a release, "1.2.3", or a pre-release of one,
# "1.2.3-beta.1" or "1.2.3-rc.2" (the label is SemVer's: dot-separated
# letters, digits and hyphens). The app shows it as written.
#
# A CIA carries its version as one 16-bit number, which AM only lets go up:
# it refuses an older number and the same one again (0xC8E083FC), and 3dsDL
# cannot remove itself while it runs to get round that. So every build a
# tester installs needs a higher number than the last, betas included, and
# there is no room for that between 0.8.0 and 0.8.1 if the CIA says 0.8.1.
#
# The number is therefore a build number made from the version:
#
#     major * 16384 + minor * 256 + patch * 16 + step
#
# where step is the pre-release's last number (1 to 14) and 15 for the
# release itself. 0.8.1-beta.1 is 2065, 0.8.1-beta.2 2066, 0.8.1-rc.3 2067
# and 0.8.1 2079. Pre-releases of one version must end in a number that
# keeps rising across labels (beta.1, beta.2, rc.3). Everything from 0.8.1
# on is far above the 128 that 0.8.0 shipped with. Major goes up to 3,
# minor to 63 and patch to 15.
#
#   scripts/version.sh                 numbers for the VERSION file
#   scripts/version.sh 0.8.1-beta.1    numbers for any version
#
# Prints "<major> <minor> <micro> <release|prerelease>", the fields makerom
# takes for that number. build.sh sources this file and calls cia_version.

cia_version() {
    local version="$1"
    if [[ ! "${version}" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-([0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*))?$ ]]; then
        echo "VERSION must look like 1.2.3 or 1.2.3-beta.1, found '${version}'." >&2
        return 1
    fi
    local major="${BASH_REMATCH[1]}" minor="${BASH_REMATCH[2]}" patch="${BASH_REMATCH[3]}"
    local label="${BASH_REMATCH[5]}"
    if (( major > 3 || minor > 63 || patch > 15 )); then
        echo "${version} does not fit a CIA's version, which holds up to 3.63.15." >&2
        return 1
    fi
    local step=15 kind=release
    if [[ -n "${label}" ]]; then
        if [[ ! "${label}" =~ (^|\.)([1-9][0-9]?)$ ]] || (( BASH_REMATCH[2] > 14 )); then
            echo "A pre-release must end in a number from 1 to 14, like beta.1 or rc.3, found '${label}'." >&2
            return 1
        fi
        step="${BASH_REMATCH[2]}"
        kind=prerelease
    fi
    local number=$(( major * 16384 + minor * 256 + patch * 16 + step ))
    echo "$(( number >> 10 )) $(( (number >> 4) & 63 )) $(( number & 15 )) ${kind}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -Eeuo pipefail
    cia_version "${1:-$(tr -d '[:space:]' < "$(dirname "$0")/../VERSION")}"
fi
