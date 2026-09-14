# Baseline Windows cross-build container.
#
# Produces the Windows deploy (1/1 parity with tools/release/ for Linux) with a
# clang-cl + lld-link toolchain: the MSVC CRT and Windows SDK are materialised
# by xwin (Jake-Shadle/xwin) and Qt comes from aqtinstall's MSVC kit, so nothing
# runs a real Visual Studio. Only the runner (ps2EntryRunner.exe) and the Qt
# launcher (Launcher.exe) are cross-compiled -- games/bt3/setup.py generates the
# recompiled sources with the native g++/clang toolchain (the codegen is
# target-agnostic) and runs on this same image.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=UTC \
    LANG=C.UTF-8 \
    XWIN_ROOT=/opt/xwin \
    WINEARCH=win64 \
    WINEPREFIX=/wine

RUN apt-get update && apt-get install -y --no-install-recommends \
        # host-side toolchain: clang-cl / lld-link come from the clang+lld+llvm
        # packages; the MSVC headers/libs are provided by xwin below.
        build-essential \
        clang \
        lld \
        llvm \
        cmake \
        ninja-build \
        make \
        git \
        ca-certificates \
        curl \
        unzip \
        zip \
        p7zip-full \
        libarchive-tools \
        file \
        ccache \
        # games/bt3/setup.py --gen-only (native recompiler + ISO extraction)
        python3 \
        python3-pip \
        # VC++ 2015-2022 redistributable extraction + optional smoke test
        wine \
        xvfb \
    && rm -rf /var/lib/apt/lists/*

# CMake: Ubuntu 22.04 ships 3.22, too old for DOWNLOAD_EXTRACT_TIMESTAMP (the
# FFmpeg ExternalProject step in ps2xRuntime). Modern CMake + aqtinstall (MSVC
# Qt kit) + pefile (PE dependency gate) from pip.
RUN pip3 install --no-cache-dir \
        cmake \
        aqtinstall \
        pefile

# ---- xwin: MSVC CRT + Windows SDK as plain files (no Visual Studio) ---------
# Pinned release; the asset name is xwin-<ver>-x86_64-unknown-linux-musl.tar.gz
# and contains a single `xwin` binary.
ARG XWIN_VERSION=0.10.0
RUN set -eux; \
    base="https://github.com/Jake-Shadle/xwin/releases/download/xwin-$XWIN_VERSION"; \
    curl -fsSL "$base/xwin-$XWIN_VERSION-x86_64-unknown-linux-musl.tar.gz" -o /tmp/xwin.tar.gz; \
    rm -rf /tmp/xwin-stage && mkdir -p /tmp/xwin-stage; \
    tar -xzf /tmp/xwin.tar.gz -C /tmp/xwin-stage; \
    mv /tmp/xwin-stage/xwin /usr/local/bin/xwin; \
    chmod +x /usr/local/bin/xwin; \
    rm -rf /tmp/xwin-stage /tmp/xwin.tar.gz

# Splat the CRT + SDK (large download). --arch x86_64 keeps the Windows SDK
# x64-only; the toolchain file below hard-codes those paths.
RUN xwin --accept-license --arch x86_64 splat --output "$XWIN_ROOT" \
    && rm -rf "$XWIN_ROOT/.xwin-cache"

# ---- Qt: aqtinstall Windows MSVC kit ----------------------------------------
RUN aqt install-qt linux desktop 6.5.3 win64_msvc2022_64 --outputdir /opt/qt

# ---- VC++ 2015-2022 runtime DLLs ---------------------------------------------
# The redistributable DLLs (vcruntime140/msvcp140/...) are NOT produced by xwin
# (headers + import libs only). They must travel with the release, so pull the
# official redistributable and let wine install a quiet x64 copy we can fish the
# DLLs out of. These same DLLs are the ones the runner/launcher import at start
# (both link /MD and Qt6 imports them).
RUN set -eux; \
    mkdir -p /opt/vcredist; \
    curl -fsSL -o /tmp/vc_redist.x64.exe https://aka.ms/vs/17/release/vc_redist.x64.exe; \
    mkdir -p "$WINEPREFIX"; \
    xvfb-run -a wineboot -u >/dev/null 2>&1 || true; \
    xvfb-run -a wine /tmp/vc_redist.x64.exe /install /quiet /norestart >/dev/null 2>&1 || true; \
    find "$WINEPREFIX/drive_c/windows/system32" -maxdepth 1 -type f \( \
        -iname 'vcruntime140.dll' -o -iname 'vcruntime140_1.dll' \
        -o -iname 'msvcp140.dll' -o -iname 'msvcp140_1.dll' -o -iname 'msvcp140_2.dll' \
        -o -iname 'concrt140.dll' -o -iname 'vcomp140.dll' \
    \) -exec cp -v -f {} /opt/vcredist/ \;; \
    rm -f /tmp/vc_redist.x64.exe; \
    test -f /opt/vcredist/vcruntime140.dll \
        -a -f /opt/vcredist/vcruntime140_1.dll \
        -a -f /opt/vcredist/msvcp140.dll; \
    ls -la /opt/vcredist/

# Never build as root inside the dev workspace: the host mounts are owned by the
# invoking user. ENTRYPOINT re-execs as that uid/gid when provided.
COPY entrypoint.sh /usr/local/bin/release-entrypoint
RUN chmod +x /usr/local/bin/release-entrypoint
ENTRYPOINT ["/usr/local/bin/release-entrypoint"]