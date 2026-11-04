#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# Pinned by digest so a re-pushed tag cannot silently change the toolchain.
BUILD_IMAGE="${THREEDSDL_BUILD_IMAGE:-devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5}"
CIA_IMAGE="${THREEDSDL_CIA_IMAGE:-3dsdl-cia-tools:1.0.2}"
BUILD_JOBS="${THREEDSDL_BUILD_JOBS:-2}"
KEEP_COLIMA="${THREEDSDL_KEEP_COLIMA:-0}"
DIST_DIR="${PROJECT_ROOT}/dist"
STARTED_COLIMA=0
STEP=0

step() {
    STEP=$((STEP + 1))
    printf '\n[%d] %s\n' "${STEP}" "$1"
}

fail() {
    printf '\nBuild failed: %s\n' "$1" >&2
    exit 1
}

cleanup() {
    if [[ "${STARTED_COLIMA}" == "1" && "${KEEP_COLIMA}" != "1" ]]; then
        printf '\nStopping the Colima instance started by this script...\n'
        colima stop >/dev/null
    fi
}

# Containers run as the calling user so build output is not owned by root on
# Linux hosts.
DOCKER_USER="$(id -u):$(id -g)"

run_builder() {
    docker run --rm \
        --user "${DOCKER_USER}" \
        --env HOME=/tmp \
        --volume "${PROJECT_ROOT}:/project" \
        --workdir /project \
        "${BUILD_IMAGE}" "$@"
}

run_cia_tool() {
    docker run --rm --platform linux/amd64 \
        --user "${DOCKER_USER}" \
        --volume "${PROJECT_ROOT}:/project" \
        --workdir /project \
        "${CIA_IMAGE}" "$@"
}

trap cleanup EXIT

[[ "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]] || fail "THREEDSDL_BUILD_JOBS must be a positive integer."

# shellcheck source=scripts/version.sh
source "${SCRIPT_DIR}/version.sh"
VERSION="$(tr -d '[:space:]' < "${PROJECT_ROOT}/VERSION")"
CIA_VERSION="$(cia_version "${VERSION}")" || fail "See VERSION."
read -r VERSION_MAJOR VERSION_MINOR VERSION_MICRO VERSION_KIND <<< "${CIA_VERSION}"
printf 'Building 3dsDL %s (%s, CIA version %s)\n' "${VERSION}" "${VERSION_KIND}" \
    "$(( (VERSION_MAJOR << 10) | (VERSION_MINOR << 4) | VERSION_MICRO ))"

step "Checking the container tools"
command -v docker >/dev/null 2>&1 || fail "Docker is required. Install Docker Desktop or Docker CLI with Colima."

if ! docker info >/dev/null 2>&1; then
    command -v colima >/dev/null 2>&1 || fail "Docker is not running and Colima is not installed."
    printf 'Docker is stopped; starting Colima...\n'
    colima start
    STARTED_COLIMA=1
    docker info >/dev/null 2>&1 || fail "Docker did not become available after Colima started."
fi

step "Checking the pinned devkitPro image (${BUILD_IMAGE})"
if ! docker image inspect "${BUILD_IMAGE}" >/dev/null 2>&1; then
    docker pull "${BUILD_IMAGE}"
else
    printf 'Using the locally cached image.\n'
fi

step "Checking the pinned CIA packaging image (${CIA_IMAGE})"
if ! docker image inspect "${CIA_IMAGE}" >/dev/null 2>&1; then
    docker build --platform linux/amd64 \
        --file "${PROJECT_ROOT}/scripts/cia-tools.Dockerfile" \
        --tag "${CIA_IMAGE}" "${PROJECT_ROOT}"
else
    printf 'Using the locally cached CIA packaging image.\n'
fi

step "Validating the launcher icon"
ICON="${PROJECT_ROOT}/resources/icon-3dsdl.png"
[[ -f "${ICON}" ]] || fail "Missing launcher icon: ${ICON}"
PNG_COLOR_TYPE="$(od -An -t u1 -j 25 -N 1 "${ICON}" | tr -d '[:space:]')"
[[ "${PNG_COLOR_TYPE}" == "4" || "${PNG_COLOR_TYPE}" == "6" ]] || \
    fail "The launcher PNG must contain a real alpha channel (PNG color type 4 or 6)."
printf 'Launcher icon contains an alpha channel.\n'

step "Cleaning previous build outputs"
run_builder make clean

step "Compiling 3dsDL with ${BUILD_JOBS} job(s)"
# THREEDSDL_EXTRA_DEFINES makes test builds, e.g. -DTHREEDSDL_FAULTS.
run_builder make -j"${BUILD_JOBS}" EXTRA_DEFINES="${THREEDSDL_EXTRA_DEFINES:-}"

step "Creating Home Menu metadata and banner"
# The HOME Menu banner is the animated 3D model from resources/banner/ (see
# scripts/make-banner.sh), confirmed on a console. THREEDSDL_FLAT_BANNER=1
# packs the flat picture instead; THREEDSDL_BANNER_CGFX packs another model.
if [[ "${THREEDSDL_FLAT_BANNER:-0}" == "1" ]]; then
    BANNER_ART=(-i resources/banner-3dsdl.png)
    printf 'Using the flat banner picture.\n'
else
    BANNER_ART=(-ci "${THREEDSDL_BANNER_CGFX:-resources/banner/3dsDL.cgfx}")
fi
run_cia_tool bannertool makebanner \
    "${BANNER_ART[@]}" \
    -a resources/banner/jingle.wav \
    -o build/3dsDL.bnr
run_cia_tool bannertool makesmdh \
    -s "3dsDL" \
    -l "Download files straight onto your 3DS" \
    -p "ankushKun" \
    -i resources/icon-3dsdl.png \
    -f visible,nosavebackups \
    -o build/3dsDL.icn

step "Packaging the installable CIA"
run_cia_tool makerom \
    -f cia -o 3dsDL.cia -target t -exefslogo \
    -elf 3dsDL.elf \
    -rsf resources/3dsDL.rsf \
    -banner build/3dsDL.bnr \
    -icon build/3dsDL.icn \
    -major "${VERSION_MAJOR}" -minor "${VERSION_MINOR}" -micro "${VERSION_MICRO}"

step "Validating build artifacts"
for artifact in 3dsDL.3dsx 3dsDL.smdh 3dsDL.cia 3dsDL.elf; do
    [[ -s "${PROJECT_ROOT}/${artifact}" ]] || fail "Missing or empty artifact: ${artifact}"
    printf 'Found %s\n' "${artifact}"
done

step "Packaging dist/ and generating SHA-256"
mkdir -p "${DIST_DIR}"
cp "${PROJECT_ROOT}/3dsDL.3dsx" "${DIST_DIR}/3dsDL.3dsx"
cp "${PROJECT_ROOT}/3dsDL.smdh" "${DIST_DIR}/3dsDL.smdh"
cp "${PROJECT_ROOT}/3dsDL.cia" "${DIST_DIR}/3dsDL.cia"
# The symbols for this build, so scripts/crash.py can name the functions in
# a crash log or a Luma3DS dump. make clean below removes the original.
cp "${PROJECT_ROOT}/3dsDL.elf" "${DIST_DIR}/3dsDL.elf"
(
    cd "${DIST_DIR}"
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 3dsDL.3dsx 3dsDL.cia > 3dsDL.sha256
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum 3dsDL.3dsx 3dsDL.cia > 3dsDL.sha256
    else
        fail "Neither shasum nor sha256sum is available."
    fi
)

step "Removing intermediate build files"
run_builder make clean

printf '\nBuild of 3dsDL %s complete.\n' "${VERSION}"
printf '  3DSX:     %s\n' "${DIST_DIR}/3dsDL.3dsx"
printf '  CIA:      %s\n' "${DIST_DIR}/3dsDL.cia"
printf '  Metadata: %s\n' "${DIST_DIR}/3dsDL.smdh"
printf '  Symbols:  %s\n' "${DIST_DIR}/3dsDL.elf"
printf '  Checksum: %s\n' "${DIST_DIR}/3dsDL.sha256"
