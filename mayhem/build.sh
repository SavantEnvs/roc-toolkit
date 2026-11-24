#!/usr/bin/env bash
# mayhem/build.sh — build roc-toolkit's libroc (sanitized + SanCov) and the fuzz harnesses, plus the
# upstream CppUTest suite with normal flags (gcc, separate build dir) for mayhem/test.sh.
# Runs offline: scons/ragel/libuv/speexdsp/CppUTest come from apt, OpenFEC from
# /opt/toolchains/deps (built by mayhem/Dockerfile); --build-3rdparty is empty so nothing downloads.
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"

cd "$SRC"
OPENFEC=$(ls -d /opt/toolchains/deps/openfec-* | head -1)
HOST=x86_64-pc-linux-gnu
COMMON_OPTS=(--build-3rdparty= --with-openfec-includes="$OPENFEC/include"
  --with-libraries="$OPENFEC/lib" --disable-sox --disable-sndfile --disable-openssl
  --disable-libunwind --disable-alsa --disable-pulseaudio --enable-static --disable-shared)

# 1) Upstream test suite, NORMAL flags, gcc (its own build dir build/src/$HOST/gcc-*-release).
env -u CFLAGS -u CXXFLAGS -u LDFLAGS scons -Q -j"$MAYHEM_JOBS" --compiler=gcc "${COMMON_OPTS[@]}" \
  --disable-tools --enable-tests CC=gcc CXX=g++ \
  CFLAGS="$COVERAGE_FLAGS" CXXFLAGS="$COVERAGE_FLAGS" LDFLAGS="$COVERAGE_FLAGS"
mkdir -p mayhem/out/tests
cp -f bin/$HOST/roc-test-* mayhem/out/tests/

# 2) libroc with ASan+UBSan (halting) + SanCov + DWARF 3 (build/src/$HOST/clang-*-release).
FUZZ_CFLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link"
scons -Q -j"$MAYHEM_JOBS" --compiler=clang "${COMMON_OPTS[@]}" --disable-tools \
  CC="$CC" CXX="$CXX" CFLAGS="$FUZZ_CFLAGS" CXXFLAGS="$FUZZ_CFLAGS" LDFLAGS="$SANITIZER_FLAGS"
# Link the relocatable object from BEFORE upstream's `objcopy --localize-hidden --strip-unneeded`
# step (src/SConscript): libroc.a is built from the stripped libroc_modules.o, which loses the DWARF
# and leaves every libroc frame without file:line. No third-party libs are composed in
# (--build-3rdparty is empty), so this object holds all of libroc.
LIBROC=$(ls -d build/src/$HOST/clang-*-release | head -1)/libroc_modules_unlocalized.o
[ -f "$LIBROC" ] || { echo "missing $LIBROC" >&2; exit 1; }

# 3) Harnesses: libFuzzer binary + standalone reproducer each.
LIBS=("$LIBROC" -L"$OPENFEC/lib" -lopenfec -lspeexdsp -luv -lpthread -ldl -lrt -lm)
INC=(-Isrc/public_api/include)
OUT=mayhem/out
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o $OUT/lsan_off.o
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o $OUT/standalone_main.o

build_harness() { # name source [extra flags...]
  local name=$1 src=$2; shift 2
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$@" "${INC[@]}" -c "$src" -o $OUT/$name.o
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE $OUT/$name.o $OUT/lsan_off.o "${LIBS[@]}" -o $OUT/$name
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $OUT/$name.o $OUT/standalone_main.o $OUT/lsan_off.o "${LIBS[@]}" -o $OUT/$name-standalone
}
build_harness fuzz_receiver_rtp    mayhem/harnesses/fuzz_receiver_decoder.cpp -DFUZZ_MODE=0
build_harness fuzz_receiver_rs8m   mayhem/harnesses/fuzz_receiver_decoder.cpp -DFUZZ_MODE=1
build_harness fuzz_receiver_ldpc   mayhem/harnesses/fuzz_receiver_decoder.cpp -DFUZZ_MODE=2
build_harness fuzz_receiver_config mayhem/harnesses/fuzz_receiver_decoder.cpp -DFUZZ_MODE=3
build_harness fuzz_sender_feedback mayhem/harnesses/fuzz_sender_feedback.cpp
build_harness fuzz_endpoint_uri    mayhem/harnesses/fuzz_endpoint_uri.cpp
ls -l $OUT
