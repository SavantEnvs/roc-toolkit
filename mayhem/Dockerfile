# syntax=docker/dockerfile:1.7
#
# Commit image for roc-toolkit (SCons build). Deps: scons/ragel/libuv/speexdsp/CppUTest from apt;
# OpenFEC (not packaged in Debian; upstream's --build-3rdparty downloads it at build time) is built
# once here from a pinned tarball into /opt/toolchains/deps/openfec-<ver> so build.sh runs offline.
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
ARG SOURCE_DATE_EPOCH
ARG SANITIZER_FLAGS
ARG COVERAGE_FLAGS=
ARG MAYHEM_JOBS=

USER root
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends \
        scons ragel libuv1-dev libspeexdsp-dev libcpputest-dev \
 && rm -rf /var/lib/apt/lists/*

# OpenFEC: the version roc's 3rdparty/SConscript pins (1.4.2.12), same cmake recipe as
# scripts/scons_helpers/build-3rdparty.py, uninstrumented, DWARF 3, static only.
ARG OPENFEC_VER=1.4.2.12
ARG OPENFEC_SHA256=3397f58c8fff945ece8ea19e7859040c98a5c6497e5d791397794094e15e5873
RUN d=/opt/toolchains/deps/openfec-${OPENFEC_VER} && mkdir -p "$d/src" "$d/include" "$d/lib" \
 && curl -fsSL -o /tmp/of.tgz https://github.com/roc-streaming/openfec/archive/v${OPENFEC_VER}.tar.gz \
 && echo "${OPENFEC_SHA256}  /tmp/of.tgz" | sha256sum -c - \
 && tar -xzf /tmp/of.tgz --strip-components=1 -C "$d/src" \
 && cp -r "$d/src" /tmp/ofb && cd /tmp/ofb && mkdir build && cd build \
 && cmake .. -DBUILD_STATIC_LIBS=ON -DDEBUG:STRING=OFF \
      -DCMAKE_C_FLAGS_RELEASE="-O2 -g -gdwarf-3 -fPIC" -DCMAKE_C_FLAGS="-g -gdwarf-3 -fPIC" \
 && make -j"$(nproc)" \
 && cp "$(find /tmp/ofb -name libopenfec.a | head -1)" "$d/lib/" \
 && cd /tmp/ofb/src && find . -name '*.h' -exec cp --parents {} "$d/include/" \; \
 && rm -rf /tmp/ofb /tmp/of.tgz && chmod -R a+rX "$d"

USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
