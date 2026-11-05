#!/bin/sh
#Builds DriftDS from your own ROM: extract, translate to C, compile, link.
#Where it fits: this is the single entry point for building. Run it in Git Bash from any folder.
#Why: the game code is translated from the ROM on your machine, so no game data is in the repository.
#
#Usage:  sh build.sh [all|extract|translate|compile|link|clean]
#Needs:  rom/game.nds (a Mario Kart DS ROM, USA or Europe), Python 3, and clang for Windows (llvm-mingw).
#Setup:  JOBS=n sets the compile parallelism (default: all cores but one). CLANG and PYTHON override the tools.
#Output: build/driftds.exe. Put settings.ini next to the folder that holds rom/ if you want to change settings.
set -e
cd "$(dirname "$0")"

STEP=${1:-all}
PYTHON=${PYTHON:-python}
CORES=${NUMBER_OF_PROCESSORS:-4}
JOBS=${JOBS:-$((CORES > 1 ? CORES - 1 : 1))}

#find clang: PATH first, then a toolchain folder next to the repository
if ! command -v "${CLANG:-clang}" >/dev/null 2>&1; then
    for bin in ./toolchain/llvm/*/bin ../toolchain/llvm/*/bin; do
        [ -x "$bin/clang.exe" ] && PATH="$(cd "$bin" && pwd):$PATH" && break
    done
fi
CLANG=${CLANG:-clang}
command -v "$CLANG" >/dev/null 2>&1 || { echo "clang not found. Install llvm-mingw and put its bin folder on PATH."; exit 1; }
export CLANG

ROM=rom/game.nds
OBJ_GEN=build/obj_gen
OBJ_RT=build/obj_rt
EXE=build/driftds.exe

step_extract() {
    [ -f "$ROM" ] || { echo "Missing $ROM. Copy your Mario Kart DS ROM there."; exit 1; }
    #skip when the extracted files are newer than the ROM
    if [ -f extracted/info.json ] && [ extracted/info.json -nt "$ROM" ]; then
        echo "extract: up to date"
        return
    fi
    "$PYTHON" -I -X utf8 tools/rom_reader.py "$ROM" extracted
}

step_translate() {
    [ -f extracted/info.json ] || step_extract
    "$PYTHON" -I -X utf8 tools/translate.py
}

#compiles every C file that is missing its object or is older than its sources
step_compile() {
    [ -d generated ] || step_translate
    mkdir -p "$OBJ_GEN" "$OBJ_RT"
    : > build/compile_jobs.txt
    #objects of folders that are no longer generated would clash at link time
    for old in "$OBJ_GEN"/*/; do
        [ -d "generated/$(basename "$old")" ] || rm -rf "$old"
    done
    for dir in generated/*/; do
        name=$(basename "$dir")
        mkdir -p "$OBJ_GEN/$name"
        for old in "$OBJ_GEN/$name"/*.o; do  #objects of files that no longer exist
            [ -f "$old" ] && [ ! -f "$dir$(basename "$old" .o).c" ] && rm -f "$old"
        done
        for src in "$dir"*.c; do
            obj="$OBJ_GEN/$name/$(basename "$src" .c).o"
            if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$dir/funcs.h" -nt "$obj" ] \
               || [ runtime/cpu.h -nt "$obj" ] || [ generated/arm9/funcs.h -nt "$obj" ]; then
                echo "$src $obj $name" >> build/compile_jobs.txt
            fi
        done
    done
    for src in runtime/*.c; do
        obj="$OBJ_RT/$(basename "$src" .c).o"
        newer_header=$(find runtime -name '*.h' -newer "$obj" 2>/dev/null | head -n 1)
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ -n "$newer_header" ]; then
            echo "$src $obj runtime" >> build/compile_jobs.txt
        fi
    done
    total=$(wc -l < build/compile_jobs.txt)
    echo "compile: $total files, $JOBS at a time"
    [ "$total" -eq 0 ] && return
    #each job line is: source object folder-name
    xargs -P "$JOBS" -L 1 sh -c '
        src=$1; obj=$2; name=$3
        flags="-O2 -w -Iruntime"
        [ "$name" != runtime ] && flags="$flags -Igenerated/$name"
        [ "$name" = arm7 ] && flags="$flags -DCPU7"
        if "$CLANG" $flags -c "$src" -o "$obj" 2> "$obj.err"; then
            rm -f "$obj.err"
        else
            echo "ERROR $src (see $obj.err)"
        fi' sh < build/compile_jobs.txt
    failed=$(find "$OBJ_GEN" "$OBJ_RT" -name '*.err' | wc -l)
    [ "$failed" -eq 0 ] || { echo "compile: $failed files failed"; exit 1; }
}

#links under a temporary name first: Windows will not replace an exe that is running
step_link() {
    find "$OBJ_GEN" "$OBJ_RT" -name '*.o' > build/link_list.txt
    "$CLANG" -O2 -w @build/link_list.txt \
        -lwinmm -lgdi32 -lopengl32 -luser32 -lxinput \
        -Wl,--stack,268435456 -o build/driftds.new.exe
    if mv -f build/driftds.new.exe "$EXE" 2>/dev/null; then
        echo "built: $EXE"
    else
        echo "built: build/driftds.new.exe (close the game, then rename it to driftds.exe)"
    fi
}

step_clean() {
    rm -rf build generated extracted
    echo "clean: build, generated and extracted removed"
}

case "$STEP" in
    all)       step_extract; step_translate; step_compile; step_link ;;
    extract)   step_extract ;;
    translate) step_translate ;;
    compile)   step_compile ;;
    link)      step_link ;;
    clean)     step_clean ;;
    *)         echo "Usage: sh build.sh [all|extract|translate|compile|link|clean]"; exit 1 ;;
esac
