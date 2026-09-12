#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PYTHON=${PYTHON:-python3}

test_environment() (
TEST_ROOT="$REPO_ROOT/build/fight_caves-tests"
MODE="${1:---core}"

case "$MODE" in
    --core|--adapter|--all) ;;
    *)
        echo "Usage: bash tests/fight_caves.sh test [--core|--adapter|--all]" >&2
        exit 2
        ;;
esac

cd "$REPO_ROOT"
"$PYTHON" -m pytest -q \
    tests/test_fight_caves.py
"$PYTHON" ocean/fight_caves/tools.py preflight --mode core

mkdir -p "$TEST_ROOT"

"${CC:-clang}" -std=c11 -O2 -Wall -Wextra -Werror \
    -Iocean/fight_caves \
    tests/fight_caves.c \
    -lm -o "$TEST_ROOT/core_contract_test"
"$TEST_ROOT/core_contract_test"

if [ "$MODE" = "--adapter" ] || [ "$MODE" = "--all" ]; then
    "$PYTHON" ocean/fight_caves/tools.py preflight --mode cpu
    ./build.sh fight_caves --cpu
    "$PYTHON" ocean/fight_caves/tools.py contract
    FC_TEST_EXECUTABLE="$REPO_ROOT/fight_caves" "$PYTHON" -m pytest -q \
        tests/test_fight_caves_runtime.py
    cmake -S ocean/fight_caves -B build/fight_caves-standard-tests \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build build/fight_caves-standard-tests \
        --target fc_integration_tests fc_cpu_tests --parallel
    build/fight_caves-standard-tests/fc_integration_tests
    build/fight_caves-standard-tests/fc_cpu_tests
fi

if [ "$MODE" = "--all" ]; then
    cmake --build build/fight_caves-standard-tests \
        --target fc_viewer_tests --parallel
    if command -v xvfb-run >/dev/null 2>&1; then
        DISPLAY_PREFIX=(xvfb-run -a)
    elif [ -n "${DISPLAY:-}" ]; then
        DISPLAY_PREFIX=()
    else
        echo "Viewer tests require xvfb-run or an existing DISPLAY." >&2
        exit 1
    fi
    (
        cd "$TEST_ROOT"
        export FC_REPO_ROOT="$REPO_ROOT"
        export FC_ASSET_ROOT="$REPO_ROOT/resources/fight_caves/viewer"
        export FC_COLLISION_PATH="$REPO_ROOT/resources/fight_caves/runtime/fightcaves.collision"
        export FC_MOVEMENT_PATH="$REPO_ROOT/resources/fight_caves/runtime/fightcaves.movement"
        export FC_LOS_PATH="$REPO_ROOT/resources/fight_caves/runtime/fightcaves.los"
        "${DISPLAY_PREFIX[@]}" "$REPO_ROOT/build/fight_caves-standard-tests/fc_viewer_tests"
    )
    "${DISPLAY_PREFIX[@]}" build/fight_caves-standard-tests/fc_integration_tests --render
fi

echo "Fight Caves environment tests passed ($MODE)."
)

COMMAND=${1:-test}
if [ "$#" -gt 0 ]; then shift; fi
case "$COMMAND" in
    test) test_environment "$@" ;;
    checkout|clean-clone) "$PYTHON" -S "$SCRIPT_DIR/fight_caves_e2e.py" "$COMMAND" "$@" ;;
    *) echo "Usage: bash tests/fight_caves.sh {test [--core|--adapter|--all]|checkout|clean-clone [--help]}" >&2; exit 2 ;;
esac
