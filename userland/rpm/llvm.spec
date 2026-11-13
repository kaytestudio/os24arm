# LLVM for UNIXROOT: clang (C, C++) and lld, compiling and linking on the
# system itself. Built by rpmbuild from the 23.1.5 release
# (userland/build-rpms.sh llvm): first the C++ runtimes for aarch64-linux-
# musl (compiler-rt builtins and crt objects, libunwind, libc++abi,
# libc++), then clang and lld against them - one static multi-call "llvm"
# program, linked in the kernel's image area (0x1_0000_0000; it's too big
# for the user window), with a launcher per tool name. The build tools
# (llvm-tblgen, clang-tblgen) are Homebrew's LLVM, which is this version.
%global llvm_major 23

Name:           llvm
Version:        23.1.5
Release:        1.os2warp
Summary:        LLVM: the clang C/C++ compiler and the lld linker
License:        Apache-2.0 WITH LLVM-exception
URL:            https://llvm.org/
Source0:        https://github.com/llvm/llvm-project/releases/download/llvmorg-%{version}/llvm-project-%{version}.src.tar.xz
Source1:        llvm-launcher.c
Source2:        aarch64-unknown-linux-musl.cfg
BuildArch:      aarch64
AutoReqProv:    no

%description
clang and lld 23.1.5 for OS/2 Warp ARM64 (aarch64, musl): "clang hello.c
-o hello", "clang++ x.cpp -o x" compile and link static programs the
system runs, with lld, compiler-rt, libc++ and libunwind by default
(/etc/clang/aarch64-unknown-linux-musl.cfg: -static, the user window).
Also llvm-ar, llvm-nm, llvm-objdump, llvm-objcopy, llvm-strip,
llvm-readelf, llvm-size. Needs musl-devel; C++ also libcxx-devel.

%package -n libcxx-devel
Summary:        libc++, libc++abi and libunwind for C++ on the system
License:        Apache-2.0 WITH LLVM-exception

%description -n libcxx-devel
LLVM's C++ standard library (libc++, with libc++abi and libunwind) for
OS/2 Warp ARM64: the headers (/usr/include/c++/v1) and static libraries
clang++ links C++ programs with.

%prep
%autosetup -n llvm-project-%{version}.src

%build
SRC="$PWD"
TT=aarch64-unknown-linux-musl
PM="-ffile-prefix-map=$SRC=/usr/src/llvm-%{version} %{os2_prefix_map}"   # (no Mac paths in __FILE__)
T="--target=aarch64-linux-musl --sysroot=%{os2_sysroot} -mno-outline-atomics"

# 1. the C++ runtimes and compiler-rt, for the system
cmake -G Ninja -S runtimes -B b-rt \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_SYSROOT=%{os2_sysroot} \
  -DCMAKE_C_COMPILER=%{os2_llvm}/clang -DCMAKE_CXX_COMPILER=%{os2_llvm}/clang++ \
  -DCMAKE_ASM_COMPILER=%{os2_llvm}/clang \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-musl -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-musl \
  -DCMAKE_ASM_COMPILER_TARGET=aarch64-linux-musl \
  -DCMAKE_C_FLAGS="-mno-outline-atomics -fno-stack-protector $PM" \
  -DCMAKE_CXX_FLAGS="-mno-outline-atomics -fno-stack-protector $PM" \
  -DCMAKE_AR=%{os2_llvm}/llvm-ar -DCMAKE_RANLIB=%{os2_llvm}/llvm-ranlib -DCMAKE_NM=%{os2_llvm}/llvm-nm \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DLLVM_ENABLE_RUNTIMES="compiler-rt;libunwind;libcxxabi;libcxx" -DLLVM_INCLUDE_TESTS=OFF \
  -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_CRT=ON -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
  -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_PROFILE=OFF \
  -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCOMPILER_RT_BUILD_ORC=OFF -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
  -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
  -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_USE_COMPILER_RT=ON -DLIBUNWIND_INCLUDE_TESTS=OFF \
  -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_USE_COMPILER_RT=ON \
  -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON -DLIBCXXABI_STATICALLY_LINK_UNWINDER_IN_STATIC_LIBRARY=ON \
  -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_USE_COMPILER_RT=ON \
  -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
  -DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
  -DLIBCXX_INCLUDE_TESTS=OFF
ninja -C b-rt -j%{os2_jobs}
DESTDIR="$SRC/stage-rt" ninja -C b-rt install

# what clang (on the Mac) builds LLVM against: musl + those runtimes,
# compiler-rt in a resource dir laid out as on the system
rm -rf sr rtdir; cp -R %{os2_sysroot} sr
cp -R stage-rt/usr/include/. sr/include/
cp stage-rt/usr/lib/*.a sr/lib/
mkdir -p rtdir/lib/$TT
cp stage-rt/usr/lib/linux/libclang_rt.builtins-aarch64.a rtdir/lib/$TT/libclang_rt.builtins.a
cp stage-rt/usr/lib/linux/clang_rt.crtbegin-aarch64.o rtdir/lib/$TT/clang_rt.crtbegin.o
cp stage-rt/usr/lib/linux/clang_rt.crtend-aarch64.o rtdir/lib/$TT/clang_rt.crtend.o
cp -R "$(%{os2_llvm}/clang -print-resource-dir)/include" rtdir/

# 2. clang + lld, one static program linked in the kernel's image area
T="--target=aarch64-linux-musl --sysroot=$SRC/sr -resource-dir $SRC/rtdir -mno-outline-atomics $PM"
cmake -G Ninja -S llvm -B b-llvm \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_SYSROOT="$SRC/sr" \
  -DCMAKE_C_COMPILER=%{os2_llvm}/clang -DCMAKE_CXX_COMPILER=%{os2_llvm}/clang++ \
  -DCMAKE_ASM_COMPILER=%{os2_llvm}/clang \
  -DCMAKE_C_FLAGS="$T" -DCMAKE_ASM_FLAGS="$T" \
  -DCMAKE_CXX_FLAGS="$T -nostdinc++ -isystem $SRC/sr/include/c++/v1 -stdlib=libc++" \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld -static -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++ -Wl,--image-base=0x100000000 -Wl,--defsym=_DYNAMIC=0x100000000" \
  -DCMAKE_AR=%{os2_llvm}/llvm-ar -DCMAKE_RANLIB=%{os2_llvm}/llvm-ranlib -DCMAKE_NM=%{os2_llvm}/llvm-nm \
  -DLLVM_HOST_TRIPLE=$TT -DLLVM_DEFAULT_TARGET_TRIPLE=$TT \
  -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON \
  -DLLVM_TABLEGEN=%{os2_llvm}/llvm-tblgen -DCLANG_TABLEGEN=%{os2_llvm}/clang-tblgen \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_PIC=OFF -DLLVM_ENABLE_THREADS=ON \
  -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_PLUGINS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF \
  -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_PLUGIN_SUPPORT=OFF \
  -DCLANG_DEFAULT_LINKER=lld -DCLANG_DEFAULT_RTLIB=compiler-rt -DCLANG_DEFAULT_CXX_STDLIB=libc++ \
  -DCLANG_DEFAULT_UNWINDLIB=libunwind -DCLANG_CONFIG_FILE_SYSTEM_DIR=/etc/clang \
  -DLLVM_APPEND_VC_REV=OFF \
  -DLLVM_PARALLEL_LINK_JOBS=1
ninja -C b-llvm -j%{os2_jobs} llvm-driver clang-resource-headers

# 3. the launchers (no symlinks here)
%{os2_cc} -O2 -o llvm-launcher %{SOURCE1}

%install
TT=aarch64-unknown-linux-musl
RES=%{buildroot}/usr/lib/clang/%{llvm_major}
install -d %{buildroot}%{_bindir} $RES/lib/$TT %{buildroot}/etc/clang
install -m 755 b-llvm/bin/llvm %{buildroot}%{_bindir}/llvm
for t in clang clang++ cc c++ ld.lld lld llvm-ar llvm-ranlib llvm-nm llvm-objdump \
         llvm-objcopy llvm-strip llvm-readelf llvm-size; do
    install -m 755 llvm-launcher %{buildroot}%{_bindir}/$t
done
cp -R b-llvm/lib/clang/%{llvm_major}/include $RES/
install -m 644 rtdir/lib/$TT/* $RES/lib/$TT/
install -m 644 %{SOURCE2} %{buildroot}/etc/clang/$TT.cfg
# libcxx-devel
install -d %{buildroot}%{_includedir} %{buildroot}%{_libdir}
cp -R stage-rt/usr/include/c++ %{buildroot}%{_includedir}/
install -m 644 stage-rt/usr/lib/libc++.a stage-rt/usr/lib/libc++abi.a \
               stage-rt/usr/lib/libc++experimental.a stage-rt/usr/lib/libunwind.a %{buildroot}%{_libdir}/
%{os2_strip}

%files
%license LICENSE.TXT
%{_bindir}/*
/usr/lib/clang
%config(noreplace) /etc/clang/aarch64-unknown-linux-musl.cfg

%files -n libcxx-devel
%license libcxx/LICENSE.TXT
%{_includedir}/c++
%{_libdir}/libc++.a
%{_libdir}/libc++abi.a
%{_libdir}/libc++experimental.a
%{_libdir}/libunwind.a

%changelog
* Fri Nov 13 2026 OS/2 Warp ARM64 build - 23.1.5-1.os2warp
- First package: clang + lld 23.1.5 (one multi-call program, launchers),
  compiler-rt, and libcxx-devel (libc++, libc++abi, libunwind).
