#!/usr/bin/env bash
# Checks the recompiler against Xenia's PowerPC instruction tests on macOS (ARM64).
#
#   ./scripts/verify.sh              # the instructions this project added
#   ./scripts/verify.sh --all        # every test Xenia has
#   ./scripts/verify.sh --fetch      # re-fetch Xenia test sources

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

ALL=0
FETCH=0
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"

for arg in "$@"; do
    case "$arg" in
        --all|-All) ALL=1 ;;
        --fetch|-Fetch) FETCH=1 ;;
        *) echo "Unknown option: $arg (use --all or --fetch)" >&2; exit 1 ;;
    esac
done

TESTS="$ROOT/tools/xenia-tests/src/xenia/cpu/ppc/testing"
TOOLS_DIR="$ROOT/tools"
BUILD="$ROOT/build"
TOOL_BUILD="$ROOT/tools/build"
HARNESS="$ROOT/tools/XenonRecomp/XenonTests"

COVERED=(
    vslh vsrah vsubshs vspltish vmaxsh vandc vminsh
    vpkswss vctuxs vavguh vsel vpkswus vsrh vrlh
    vpkshss vpkuhus
)

is_covered() {
    local target="$1"
    for op in "${COVERED[@]}"; do
        if [[ "$target" == "instr_$op" || "$target" == "$op" ]]; then
            return 0
        fi
    done
    return 1
}

# --- 1. Test sources -------------------------------------------------------
if [[ "$FETCH" -eq 1 || ! -f "$TESTS/instr_add.s" ]]; then
    echo "==> Fetching Xenia instruction tests"
    CLONE="$TOOLS_DIR/xenia-tests"
    rm -rf "$CLONE"
    mkdir -p "$CLONE"
    git -C "$CLONE" init -q .
    git -C "$CLONE" remote add origin https://github.com/xenia-project/xenia.git
    git -C "$CLONE" config core.sparseCheckout true
    echo "src/xenia/cpu/ppc/testing/*" > "$CLONE/.git/info/sparse-checkout"
    git -C "$CLONE" fetch --depth 1 origin master -q
    git -C "$CLONE" checkout -q FETCH_HEAD
fi

for op in "${COVERED[@]}"; do
    if [[ ! -f "$TESTS/instr_$op.s" ]]; then
        echo "Error: missing test source for covered test: $TESTS/instr_$op.s" >&2
        exit 1
    fi
done

# --- 2. Tools --------------------------------------------------------------
echo "==> Building the assembler and the recompiler"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target PpcAsm --parallel "$JOBS"
cmake -S "$ROOT/tools/XenonRecomp" -B "$TOOL_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$TOOL_BUILD" --target XenonRecomp --parallel "$JOBS"

# --- 3. Assemble -----------------------------------------------------------
echo "==> Assembling"
BIN="$TESTS/bin"
rm -rf "$BIN"
mkdir -p "$BIN"
ASM="$BUILD/PpcAsm/PpcAsm"

clean=0
problems=0
skipped_asm=()
failed_covered=()

for source in "$TESTS"/*.s; do
    name="$(basename "$source" .s)"
    set +e
    asm_out="$("$ASM" "$source" "$BIN/$name.o" "$BIN/$name.dis" 0x10000 2>&1)"
    asm_status=$?
    set -e
    if [[ $asm_status -eq 0 && -f "$BIN/$name.o" && -f "$BIN/$name.dis" ]]; then
        clean=$((clean + 1))
    else
        rm -f "$BIN/$name.o" "$BIN/$name.dis"
        problems=$((problems + 1))
        skipped_asm+=("$name")
        if is_covered "$name"; then
            failed_covered+=("$name: $asm_out")
        fi
    fi
done

if [[ ${#failed_covered[@]} -gt 0 ]]; then
    echo "Error: covered test(s) failed to assemble:" >&2
    for err in "${failed_covered[@]}"; do
        echo "  $err" >&2
    done
    exit 1
fi

echo "    $clean assembled cleanly, $problems had instructions the assembler cannot encode"

# --- 4. Select -------------------------------------------------------------
if [[ "$ALL" -eq 1 ]]; then
    SELECTED="$TESTS/bin"
else
    SELECTED="$TESTS/bin-covered"
    rm -rf "$SELECTED"
    mkdir -p "$SELECTED"
    for op in "${COVERED[@]}"; do
        for ext in o dis; do
            file="$BIN/instr_$op.$ext"
            if [[ ! -f "$file" ]]; then
                echo "Error: covered test file missing: $file" >&2
                exit 1
            fi
            cp "$file" "$SELECTED/"
        done
    done
    count="$(find "$SELECTED" -maxdepth 1 -name '*.o' | wc -l | tr -d ' ')"
    if (( count != ${#COVERED[@]} )); then
        echo "Error: expected ${#COVERED[@]} covered tests, found $count" >&2
        exit 1
    fi
    echo "    testing the $count instructions this project added"
fi

# --- 5. Recompile and run --------------------------------------------------
echo "==> Recompiling the tests"
rm -f "$HARNESS"/*.cpp

set +e
recomp_output="$("$TOOL_BUILD/XenonRecomp/XenonRecomp" "$SELECTED" "$HARNESS" 2>&1)"
recomp_status=$?
set -e

skipped_fns="$(printf "%s\n" "$recomp_output" | grep -E "has unimplemented instructions|Unrecognized instruction" || true)"
missing_syms="$(printf "%s\n" "$recomp_output" | grep "Found no symbol" || true)"
missing_srcs="$(printf "%s\n" "$recomp_output" | grep "Unable to locate" || true)"

if [[ $recomp_status -ne 0 || -n "$skipped_fns" || -n "$missing_syms" || -n "$missing_srcs" ]]; then
    echo "Error: test recompilation failed or omitted tests during generation:" >&2
    if [[ -n "$skipped_fns" ]]; then
        printf "%s\n" "$skipped_fns" | sed 's/^/  /' >&2
    fi
    if [[ -n "$missing_syms" ]]; then
        printf "%s\n" "$missing_syms" | sed 's/^/  /' >&2
    fi
    if [[ -n "$missing_srcs" ]]; then
        printf "%s\n" "$missing_srcs" | sed 's/^/  /' >&2
    fi
    if [[ $recomp_status -ne 0 && -z "$skipped_fns" && -z "$missing_syms" && -z "$missing_srcs" ]]; then
        printf "%s\n" "$recomp_output" | sed 's/^/  /' >&2
    fi
    exit 1
fi

if [[ ! -f "$HARNESS/main.cpp" ]]; then
    echo "Error: test harness main.cpp was not generated" >&2
    exit 1
fi

test_count="$(grep -cE '^\s*test_[a-zA-Z0-9_]+\(base\);' "$HARNESS/main.cpp" || true)"
if (( test_count == 0 )); then
    echo "Error: test runner is empty (no tests generated)" >&2
    exit 1
fi

if [[ "$ALL" -eq 0 ]]; then
    for op in "${COVERED[@]}"; do
        if ! grep -qE "^\s*test_${op}_" "$HARNESS/main.cpp"; then
            echo "Error: no tests generated for covered instruction $op" >&2
            exit 1
        fi
    done
fi

echo "==> Building and running"
cmake -S "$ROOT/tools/XenonRecomp" -B "$TOOL_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$TOOL_BUILD" --target XenonTests --parallel "$JOBS"

set +e
output="$("$TOOL_BUILD/XenonTests/XenonTests" 2>&1)"
status=$?
set -e
failures="$(printf "%s\n" "$output" | grep "EXPECTED" || true)"

echo ""
if [[ -n "$failures" ]]; then
    fail_count="$(printf "%s\n" "$failures" | wc -l | tr -d ' ')"
    echo "$fail_count checks failed:"
    printf "%s\n" "$failures" | head -n 40 | sed 's/^/  /'
fi
if (( status != 0 )); then
    echo "XenonTests exited with status $status (crash or trap?). Last output:"
    printf "%s\n" "$output" | tail -n 20 | sed 's/^/  /'
fi
if [[ -n "$failures" ]] || (( status != 0 )); then
    exit 1
fi

if [[ "$ALL" -eq 1 ]]; then
    echo "Every check passed: all $test_count tests passed across $clean instruction test files."
    echo "Intentional assembly skips: $problems tests (instructions the assembler cannot encode)."
else
    echo "Every check passed: all $test_count tests passed across ${#COVERED[@]} covered instructions."
fi
