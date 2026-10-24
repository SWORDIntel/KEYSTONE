#!/usr/bin/env bash
# scripts/install_keystone.sh — Automated Host ISA Detection, Build & Installer for KEYSTONE
#
# Reports host CPU ISA extensions and lets the Makefile select complete ISA
# groups in auto mode. Compiles the library, search tools, and daemon,
# installs binaries and libraries into /opt/KEYSTONE, and configures, enables, and launches
# the systemd keystoned service.
#
# Usage:
#   sudo ./scripts/install_keystone.sh [options]
#   ./scripts/install_keystone.sh --dry-run
#
# SPDX-License-Identifier: LicenseRef-SRSL-1.0

set -euo pipefail

# ── Configuration Constants ───────────────────────────────────────────────────
DEFAULT_PREFIX="/opt/KEYSTONE"
DEFAULT_SOCKET="/run/keystone/keystoned.sock"
DEFAULT_SERVICE="keystoned"
DEFAULT_EPOCH=1

# ── Path Resolution ───────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Privilege Detection & Target User Resolution ──────────────────────────────
IS_ROOT=0
[ "$(id -u)" -eq 0 ] && IS_ROOT=1
SUDO=""

# Resolve target owner: if run via sudo, install for that user; if run directly as root, install as root
TARGET_USER="${SUDO_USER:-root}"
TARGET_GROUP="${TARGET_USER}"
if [ "$TARGET_USER" != "root" ]; then
    TARGET_GROUP="$(id -gn "${TARGET_USER}" 2>/dev/null || echo "${TARGET_USER}")"
fi

# ── Options & Defaults ────────────────────────────────────────────────────────
PREFIX="$DEFAULT_PREFIX"
SOCKET_PATH="$DEFAULT_SOCKET"
SERVICE_NAME="$DEFAULT_SERVICE"
EPOCH="$DEFAULT_EPOCH"
QIHSE_ROOT_OVERRIDE=""

JOBS=""
DO_CLEAN=1       # Requirement 1: installer must run make clean by default
DO_SERVICE=1     # Requirement 2: systemd service enabled by default
DO_LAUNCH=1      # Requirement 2: launch by default
VERBOSE=1        # Requirement 2: verbose by default
DRY_RUN=0

# ── Usage / Help ──────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
KEYSTONE Host Installer & Hardware-Optimized Provisioner

Usage: $(basename "$0") [options]

Installation Options:
  --prefix DIR             Installation directory prefix (default: ${DEFAULT_PREFIX})
  --socket PATH            Unix domain socket path for keystoned (default: ${DEFAULT_SOCKET})
  --epoch N                Initial fencing epoch (default: ${DEFAULT_EPOCH})
  --qihse-root DIR         Path to QIHSE installation (default: auto-detected at /opt/QIHSE or ../QIHSE)

Build & Execution Options:
  --jobs N, -j N           Number of parallel build threads (default: half host cores)
  --clean                  Run 'make clean' before compiling (default: active)
  --no-clean               Skip 'make clean' before compiling
  --no-service             Install binaries and libraries, but do not create/modify systemd unit
  --launch                 Start/restart systemd keystoned service immediately (default: active)
  --no-launch              Create systemd unit and enable on boot, but leave daemon offline
  --dry-run                Print detected ISA, planned compiler flags, and install plan without modifying system
  --verbose, -v            Show detailed command output (default: active)
  --quiet, -q              Suppress non-essential output
  --help, -h               Show this help message

EOF
    exit 0
}

# ── Argument Parsing ──────────────────────────────────────────────────────────
while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)            PREFIX="$2"; shift 2 ;;
        --socket)            SOCKET_PATH="$2"; shift 2 ;;
        --epoch)             EPOCH="$2"; shift 2 ;;
        --qihse-root)        QIHSE_ROOT_OVERRIDE="$2"; shift 2 ;;
        --jobs|-j)           JOBS="$2"; shift 2 ;;
        --clean)             DO_CLEAN=1; shift ;;
        --no-clean)          DO_CLEAN=0; shift ;;
        --no-service)        DO_SERVICE=0; shift ;;
        --launch)            DO_LAUNCH=1; shift ;;
        --no-launch)         DO_LAUNCH=0; shift ;;
        --dry-run)           DRY_RUN=1; shift ;;
        --verbose|-v)        VERBOSE=1; shift ;;
        --quiet|-q)          VERBOSE=0; shift ;;
        --help|-h)           usage ;;
        *)
            echo "[ERROR] Unknown option: $1" >&2
            echo "Run with --help for usage." >&2
            exit 1
            ;;
    esac
done

# Privilege check: real system installation requires root, but allow dry-run and help
if [ "$DRY_RUN" -eq 0 ] && [ "$IS_ROOT" -eq 0 ]; then
    echo ""
    echo "======================================================================"
    echo " [ERROR] Root privileges required for system installation."
    echo " Please rerun with sudo:"
    echo "     sudo $0 $*"
    echo "======================================================================"
    echo ""
    read -r -p "Press [Enter] to exit..." _ || true
    exit 1
fi

SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

echo "╔══════════════════════════════════════════════════════════════════════╗"
echo "║                  KEYSTONE HOST HARDWARE INSTALLER                    ║"
echo "║          HARDWARE ISA DETECTION, BUILD & HOST PROVISIONING           ║"
echo "╚══════════════════════════════════════════════════════════════════════╝"

# ── Step 1: Detect Host Hardware & CPU ISA Extensions ─────────────────────────
echo ""
echo "[1/5] Detecting CPU ISA Extensions and Hardware Topology..."

ARCH="$(uname -m)"
NPROC="$(nproc 2>/dev/null || echo 4)"
CPU_MODEL="Unknown CPU"

if [ -r /proc/cpuinfo ]; then
    CPU_MODEL="$(awk -F: '/model name/{sub(/^ /, "", $2); print $2; exit}' /proc/cpuinfo || echo "Unknown CPU")"
fi

echo "  • Machine Architecture : ${ARCH}"
echo "  • CPU Model            : ${CPU_MODEL}"
echo "  • Logical Cores        : ${NPROC}"

# Allocate 1/2 of available cores to maintain host responsiveness
if [ -z "$JOBS" ]; then
    JOBS=$(( (NPROC + 1) / 2 ))
    [ "$JOBS" -lt 1 ] && JOBS=1
fi
echo "  • Build Concurrency    : ${JOBS} threads (-j ${JOBS})"

# Extract CPU flags
FLAGS_LIST=""
if [ -r /proc/cpuinfo ]; then
    FLAGS_LIST="$(awk -F: '/^(flags|Features)[[:space:]]*:/{sub(/^ /, "", $2); print $2; exit}' /proc/cpuinfo || true)"
fi
if [ -z "$FLAGS_LIST" ] && command -v lscpu >/dev/null 2>&1; then
    FLAGS_LIST="$(lscpu | awk -F: '/^(Flags|Features):/{sub(/^ /, "", $2); print $2; exit}' || true)"
fi

has_flag() {
    case " ${FLAGS_LIST} " in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

KEYSTONE_ENABLE_OPENMP=0
KEYSTONE_ENABLE_FORTRAN=0
KEYSTONE_ENABLE_CUDA=0
KEYSTONE_ENABLE_TAR_ZST=0

HAS_AVX=0
HAS_AVX2=0
HAS_FMA=0
HAS_AVX512F=0
HAS_AVX512DQ=0
HAS_AVX512BW=0
HAS_AVX512VL=0
HAS_AVX_VNNI=0
HAS_AMX=0
HAS_OPENMP=0
HAS_FORTRAN=0
HAS_CUDA=0
HAS_TAR_ZST=0

if [ "$ARCH" = "x86_64" ]; then
    has_flag "avx" && HAS_AVX=1
    has_flag "fma" && HAS_FMA=1
    has_flag "avx2" && HAS_AVX2=1

    has_flag "avx512f"  && HAS_AVX512F=1
    has_flag "avx512dq" && HAS_AVX512DQ=1
    has_flag "avx512bw" && HAS_AVX512BW=1
    has_flag "avx512vl" && HAS_AVX512VL=1

    if has_flag "avx_vnni" && [ "$HAS_AVX2" -eq 1 ]; then HAS_AVX_VNNI=1; fi
    if has_flag "amx_tile" && has_flag "amx_int8" && has_flag "amx_bf16"; then HAS_AMX=1; fi

    echo "  • CPU Instruction Sets :"
    [ "$HAS_AVX" -eq 1 ]      && echo "    [✓] AVX             (256-bit Vector Floating Point)" || echo "    [ ] AVX             (Not supported)"
    [ "$HAS_AVX2" -eq 1 ] && [ "$HAS_FMA" -eq 1 ] && echo "    [✓] AVX2 + FMA      (256-bit Vector Integer & FMA)" || echo "    [ ] AVX2 + FMA      (Incomplete feature group)"
    [ "$HAS_AVX512F" -eq 1 ]  && echo "    [✓] AVX-512 F       (512-bit Foundation)" || echo "    [ ] AVX-512 F       (Not supported)"
    [ "$HAS_AVX512DQ" -eq 1 ] && echo "    [✓] AVX-512 DQ      (512-bit Doubleword & Quadword)" || echo "    [ ] AVX-512 DQ      (Not supported)"
    [ "$HAS_AVX512BW" -eq 1 ] && echo "    [✓] AVX-512 BW      (512-bit Byte & Word)" || echo "    [ ] AVX-512 BW      (Not supported)"
    [ "$HAS_AVX512VL" -eq 1 ] && echo "    [✓] AVX-512 VL      (512-bit Vector Length Extensions)" || echo "    [ ] AVX-512 VL      (Not supported)"
    [ "$HAS_AVX_VNNI" -eq 1 ] && echo "    [✓] AVX-VNNI        (256-bit integer dot product)" || echo "    [ ] AVX-VNNI        (Not supported)"
    [ "$HAS_AMX" -eq 1 ]      && echo "    [✓] AMX             (Tile, INT8, BF16)" || echo "    [ ] AMX             (Incomplete feature group)"

elif [ "$ARCH" = "aarch64" ]; then
    HAS_NEON=0; has_flag "asimd" && HAS_NEON=1
    HAS_SVE=0;  has_flag "sve" && HAS_SVE=1
    echo "  • ARM Extensions       :"
    [ "$HAS_NEON" -eq 1 ] && echo "    [✓] NEON/ASIMD      (Advanced SIMD)"
    [ "$HAS_SVE" -eq 1 ]  && echo "    [✓] SVE             (Scalable Vector Extension)"
fi

echo "  • Compilers & Accelerators :"
if echo | ${CC:-gcc} -fopenmp -dM -E - 2>/dev/null | grep -q '_OPENMP'; then
    HAS_OPENMP=1
    KEYSTONE_ENABLE_OPENMP=1
    echo "    [✓] OpenMP          (Multi-threaded parallel search)"
else
    echo "    [ ] OpenMP          (Compiler does not support OpenMP)"
fi

if command -v gfortran >/dev/null 2>&1; then
    HAS_FORTRAN=1
    KEYSTONE_ENABLE_FORTRAN=1
    GFORTRAN_VER="$(gfortran --version 2>/dev/null | head -n1)"
    echo "    [✓] Fortran         (${GFORTRAN_VER})"
else
    echo "    [ ] Fortran         (gfortran not found)"
fi

if command -v nvcc >/dev/null 2>&1; then
    HAS_CUDA=1
    KEYSTONE_ENABLE_CUDA=1
    CUDA_VER="$(nvcc --version 2>/dev/null | grep -o 'release [0-9.]*' || echo "nvcc present")"
    echo "    [✓] CUDA / GPU      (${CUDA_VER})"
else
    echo "    [ ] CUDA / GPU      (nvcc not found)"
fi

if pkg-config --exists libarchive libzstd 2>/dev/null; then
    HAS_TAR_ZST=1
    KEYSTONE_ENABLE_TAR_ZST=1
    echo "    [✓] libarchive/zstd (Streaming .tar.zst archive triage)"
else
    echo "    [ ] libarchive/zstd (libarchive / libzstd development libraries not found)"
fi

# ── Step 2: Auto-Detect QIHSE Installation & Bridge Configuration ────────────
echo ""
echo "[2/5] Detecting QIHSE Integration & Bridge..."

KEYSTONE_ENABLE_QIHSE_BRIDGE=0
QIHSE_ROOT=""

CANDIDATES=()
if [ -n "$QIHSE_ROOT_OVERRIDE" ]; then
    CANDIDATES+=("$QIHSE_ROOT_OVERRIDE")
elif [ -n "${QIHSE_ROOT:-}" ]; then
    CANDIDATES+=("$QIHSE_ROOT")
fi
CANDIDATES+=(
    "/opt/QIHSE"
    "${ROOT_DIR}/../QIHSE"
    "/home/john/Documents/QIHSE"
)

for cand in "${CANDIDATES[@]}"; do
    if [ -d "$cand" ]; then
        if { [ -f "$cand/include/qihse.h" ] || [ -f "$cand/include/qihse_kv_store.h" ] || [ -f "$cand/qihse.h" ]; } && \
           { [ -f "$cand/libqihse.so" ] || [ -f "$cand/lib/libqihse.so" ]; }; then
            QIHSE_ROOT="$(cd "$cand" && pwd)"
            KEYSTONE_ENABLE_QIHSE_BRIDGE=1
            break
        fi
    fi
done

if [ "$KEYSTONE_ENABLE_QIHSE_BRIDGE" -eq 1 ]; then
    echo "  • QIHSE Installation  : ${QIHSE_ROOT}"
    echo "  • QIHSE Bridge Mode   : [✓] Configured (KEYSTONE_ENABLE_QIHSE_BRIDGE=1, QIHSE_ROOT=${QIHSE_ROOT})"
else
    echo "  • QIHSE Installation  : Not found"
    echo "  • QIHSE Bridge Mode   : [ ] Standalone mode (KEYSTONE_ENABLE_QIHSE_BRIDGE=0)"
fi

# ── Step 3: Handle Dry Run ───────────────────────────────────────────────────
if [ "$DRY_RUN" -eq 1 ]; then
    echo ""
    echo "[DRY RUN] Configured Parameters & Installation Plan:"
    echo "  Prefix:                 ${PREFIX}"
    echo "  Target User:            ${TARGET_USER}:${TARGET_GROUP}"
    echo "  Bin Directory:          ${PREFIX}/bin"
    echo "  Lib Directory:          ${PREFIX}/lib"
    echo "  Include Directory:      ${PREFIX}/include/keystone"
    if [ "$HAS_CUDA" -eq 1 ]; then
        echo "  CUDA Directory:         ${PREFIX}/cuda"
    fi
    echo "  Systemd Service:        $( [ "$DO_SERVICE" -eq 1 ] && echo "${SERVICE_FILE} (enabled on boot; multi-user.target)" || echo "None (--no-service)" )"
    echo "  Auto-Start on Boot:     Enabled (multi-user.target; active regardless of user login)"
    echo "  Service Socket:         ${SOCKET_PATH}"
    echo "  Launch Service:         $( [ "$DO_LAUNCH" -eq 1 ] && echo "Yes" || echo "No (--no-launch)" )"
    echo "  Clean Before Build:     $( [ "$DO_CLEAN" -eq 1 ] && echo "Yes (make clean default)" || echo "No (--no-clean)" )"
    echo "  Build Concurrency:      ${JOBS} threads (-j ${JOBS})"
    echo "  KEYSTONE_ISA:           auto (Makefile checks complete host feature groups)"
    echo "  KEYSTONE_ENABLE_OPENMP: ${KEYSTONE_ENABLE_OPENMP}"
    echo "  KEYSTONE_ENABLE_FORTRAN:${KEYSTONE_ENABLE_FORTRAN}"
    echo "  KEYSTONE_ENABLE_CUDA:   ${KEYSTONE_ENABLE_CUDA}"
    echo "  KEYSTONE_ENABLE_TAR_ZST:${KEYSTONE_ENABLE_TAR_ZST}"
    echo "  QIHSE Bridge:           ${KEYSTONE_ENABLE_QIHSE_BRIDGE} (QIHSE_ROOT=${QIHSE_ROOT:-None})"
    echo ""
    echo "[DRY RUN] Planned Make Invocation:"
    if [ "$DO_CLEAN" -eq 1 ]; then
        echo "  make clean"
    fi
    echo "  make -j ${JOBS} \\"
    echo "    KEYSTONE_ISA=auto \\"
    echo "    KEYSTONE_ENABLE_OPENMP=${KEYSTONE_ENABLE_OPENMP} \\"
    echo "    KEYSTONE_ENABLE_FORTRAN=${KEYSTONE_ENABLE_FORTRAN} \\"
    echo "    KEYSTONE_ENABLE_CUDA=${KEYSTONE_ENABLE_CUDA} \\"
    echo "    KEYSTONE_ENABLE_TAR_ZST=${KEYSTONE_ENABLE_TAR_ZST} \\"
    echo "    KEYSTONE_ENABLE_QIHSE_BRIDGE=${KEYSTONE_ENABLE_QIHSE_BRIDGE} \\"
    if [ "$KEYSTONE_ENABLE_QIHSE_BRIDGE" -eq 1 ]; then
        echo "    QIHSE_ROOT=${QIHSE_ROOT} \\"
    fi
    echo "    all"
    echo ""
    echo "[DRY RUN] Planned Systemd Unit (${SERVICE_FILE}):"
    cat <<EOF
[Unit]
Description=KEYSTONE Vector Engine Service Daemon (keystoned)
Documentation=https://github.com/SWORDIntel/KEYSTONE
After=network.target

[Service]
Type=simple
ExecStart=${PREFIX}/bin/keystoned -s ${SOCKET_PATH}
RuntimeDirectory=keystone
RuntimeDirectoryMode=0755
Environment=LD_LIBRARY_PATH=${PREFIX}/lib:${PREFIX}/cuda:${PREFIX}/bin$( [ -n "$QIHSE_ROOT" ] && echo ":${QIHSE_ROOT}/lib:${QIHSE_ROOT}" )
Restart=always
RestartSec=3
KillMode=mixed
TimeoutStopSec=10
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF
    echo ""
    echo "[DRY RUN] Exiting cleanly without making any changes to the system."
    exit 0
fi

# ── Step 4: Build KEYSTONE Suite ─────────────────────────────────────────────
echo ""
echo "[3/5] Compiling Hardware-Optimized KEYSTONE Suite..."

cd "${ROOT_DIR}"

if [ "$DO_CLEAN" -eq 1 ]; then
    echo "  • Running 'make clean' (always clean by default)..."
    make clean
fi

echo "  • Compiling with detected ISA and hardware parameters..."
MAKE_ARGS=(
    "-j" "${JOBS}"
    "KEYSTONE_ISA=auto"
    "KEYSTONE_ENABLE_OPENMP=${KEYSTONE_ENABLE_OPENMP}"
    "KEYSTONE_ENABLE_FORTRAN=${KEYSTONE_ENABLE_FORTRAN}"
    "KEYSTONE_ENABLE_CUDA=${KEYSTONE_ENABLE_CUDA}"
    "KEYSTONE_ENABLE_TAR_ZST=${KEYSTONE_ENABLE_TAR_ZST}"
    "KEYSTONE_ENABLE_QIHSE_BRIDGE=${KEYSTONE_ENABLE_QIHSE_BRIDGE}"
)
if [ "$KEYSTONE_ENABLE_QIHSE_BRIDGE" -eq 1 ] && [ -n "$QIHSE_ROOT" ]; then
    MAKE_ARGS+=("QIHSE_ROOT=${QIHSE_ROOT}")
fi

make "${MAKE_ARGS[@]}" all

# Verify build outputs
REQUIRED_ARTIFACTS=(
    "libkeystone.so"
    "bin/keystoned"
    "bin/tgrep"
)
for art in "${REQUIRED_ARTIFACTS[@]}"; do
    if [ ! -f "${ROOT_DIR}/${art}" ]; then
        echo "[ERROR] Expected build output '${art}' was not created!" >&2
        exit 1
    fi
done
echo "  [✓] Compilation completed successfully."

# ── Step 5: Install Binaries, Shared Libraries and Headers into PREFIX ───────
echo ""
echo "[4/5] Installing Binaries, Shared Libraries and Headers into ${PREFIX}..."

$SUDO install -d -m 755 "${PREFIX}/bin"
$SUDO install -d -m 755 "${PREFIX}/lib"
$SUDO install -d -m 755 "${PREFIX}/include/keystone"

backup_if_exists() {
    local target="$1"
    if [ -f "$target" ]; then
        local ts
        ts="$(date +%Y%m%d-%H%M%S)"
        $SUDO cp -p "$target" "${target}.bak-${ts}"
    fi
}

safe_install() {
    local mode="$1"
    local src="$2"
    local dst="$3"

    if [ -e "$dst" ] && [ "$(readlink -f "$src")" = "$(readlink -f "$dst")" ]; then
        $SUDO chmod "$mode" "$dst"
        return 0
    fi

    if [ -d "$dst" ]; then
        local target_file="${dst}/$(basename "$src")"
        if [ -e "$target_file" ] && [ "$(readlink -f "$src")" = "$(readlink -f "$target_file")" ]; then
            $SUDO chmod "$mode" "$target_file"
            return 0
        fi
        backup_if_exists "$target_file"
        $SUDO install -m "$mode" "$src" "$target_file"
    else
        backup_if_exists "$dst"
        $SUDO install -m "$mode" "$src" "$dst"
    fi
}

# Install core shared library
echo "  • Installing shared libraries..."
safe_install 755 "${ROOT_DIR}/libkeystone.so" "${PREFIX}/lib/libkeystone.so"
safe_install 755 "${ROOT_DIR}/libkeystone.so" "${PREFIX}/bin/libkeystone.so"

if [ -f "${ROOT_DIR}/cuda/libkeystone_cuda.so" ]; then
    $SUDO install -d -m 755 "${PREFIX}/cuda"
    safe_install 755 "${ROOT_DIR}/cuda/libkeystone_cuda.so" "${PREFIX}/cuda/libkeystone_cuda.so"
    safe_install 755 "${ROOT_DIR}/cuda/libkeystone_cuda.so" "${PREFIX}/lib/libkeystone_cuda.so"
fi

# Install CLI binaries
echo "  • Installing CLI binaries..."
safe_install 755 "${ROOT_DIR}/bin/keystoned" "${PREFIX}/bin/keystoned"
if [ -f "${ROOT_DIR}/bin/tgrep" ]; then
    safe_install 755 "${ROOT_DIR}/bin/tgrep" "${PREFIX}/bin/tgrep"
fi

# Install C headers
echo "  • Installing C headers into ${PREFIX}/include/keystone..."
for h in "${ROOT_DIR}"/include/*.h; do
    if [ -f "$h" ]; then
        safe_install 644 "$h" "${PREFIX}/include/keystone/$(basename "$h")"
    fi
done

echo "  [✓] Binaries, libraries, and headers installed into ${PREFIX}."

# Ensure user ownership of installed files in /opt when running via sudo
if [ "$TARGET_USER" != "root" ] && [ -d "${PREFIX}" ]; then
    chown -R "${TARGET_USER}:${TARGET_GROUP}" "${PREFIX}"
    echo "  • Configured ${PREFIX} ownership for user ${TARGET_USER}:${TARGET_GROUP}."
fi

# ── Step 6: Configure & Install Systemd Unit ─────────────────────────────────
echo ""
echo "[5/5] Configuring Systemd Service & Boot Enablement..."

if [ "$DO_SERVICE" -eq 1 ]; then
    SOCKET_DIR="$(dirname "${SOCKET_PATH}")"
    $SUDO install -d -m 755 "${SOCKET_DIR}"

    LD_PATHS="${PREFIX}/lib:${PREFIX}/cuda:${PREFIX}/bin"
    if [ -n "$QIHSE_ROOT" ]; then
        LD_PATHS="${LD_PATHS}:${QIHSE_ROOT}/lib:${QIHSE_ROOT}"
    fi

    # Backup existing service if present
    if [ -f "$SERVICE_FILE" ]; then
        backup_if_exists "$SERVICE_FILE"
    fi

    echo "  • Writing service definition to ${SERVICE_FILE}..."
    cat <<EOF | $SUDO tee "$SERVICE_FILE" >/dev/null
[Unit]
Description=KEYSTONE Vector Engine Service Daemon (keystoned)
Documentation=https://github.com/SWORDIntel/KEYSTONE
After=network.target

[Service]
Type=simple
ExecStart=${PREFIX}/bin/keystoned -s ${SOCKET_PATH}
RuntimeDirectory=keystone
RuntimeDirectoryMode=0755
Environment=LD_LIBRARY_PATH=${LD_PATHS}
Restart=always
RestartSec=3
KillMode=mixed
TimeoutStopSec=10
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF

    echo "  • Reloading systemd daemon..."
    $SUDO systemctl daemon-reload

    echo "  • Enabling service '${SERVICE_NAME}' to launch on boot..."
    $SUDO systemctl enable "${SERVICE_NAME}"
    echo "  [✓] Service '${SERVICE_NAME}' enabled to start on system boot (multi-user.target; runs regardless of logged-in user)."

    if [ "$DO_LAUNCH" -eq 1 ]; then
        echo "  • Starting/Restarting service '${SERVICE_NAME}'..."
        $SUDO systemctl restart "${SERVICE_NAME}"

        echo "  • Verifying daemon startup and socket health..."
        HEALTHY=0
        for _ in $(seq 1 15); do
            sleep 1
            if systemctl is-active --quiet "${SERVICE_NAME}" && [ -S "${SOCKET_PATH}" ]; then
                HEALTHY=1
                break
            fi
        done

        if [ "$HEALTHY" -eq 1 ]; then
            echo "  [✓] Service '${SERVICE_NAME}' is ACTIVE and listening on ${SOCKET_PATH}."
            echo "  [✓] Launch on boot enabled (multi-user.target)."
        else
            echo "  [!] Warning: Service started but socket verification timed out. Check: journalctl -u ${SERVICE_NAME}"
        fi
    else
        echo "  • Service configured and enabled on boot (--no-launch specified, daemon left offline)."
    fi
else
    echo "  • Skipping systemd service configuration (--no-service specified)."
fi

echo ""
echo "════════════════════════════════════════════════════════════════════════"
echo "  KEYSTONE Host Installation Complete!"
echo "  • Binaries: ${PREFIX}/bin"
echo "  • Libs:     ${PREFIX}/lib"
echo "  • Headers:  ${PREFIX}/include/keystone"
if [ "$DO_SERVICE" -eq 1 ]; then
    echo "  • Service:  ${SERVICE_NAME} (socket: ${SOCKET_PATH})"
fi
echo "════════════════════════════════════════════════════════════════════════"
