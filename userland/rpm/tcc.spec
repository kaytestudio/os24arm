# tcc for UNIXROOT: the Tiny C Compiler, compiling and linking C on the
# system itself (with musl-devel). Built by rpmbuild from the 0.9.27
# release (userland/build-rpms.sh tcc): first a cross tcc for the Mac,
# which builds libtcc1.a, then the tcc that runs on the system.
Name:           tcc
Version:        0.9.27
Release:        1.os2warp
Summary:        Tiny C Compiler: compiles and links C programs on the system
License:        LGPL-2.1-or-later
URL:            https://bellard.org/tcc/
Source0:        https://download.savannah.gnu.org/releases/tinycc/tcc-%{version}.tar.bz2
Patch0:         tcc-0.9.27-os2warp.patch
BuildArch:      aarch64
AutoReqProv:    no

%description
tcc 0.9.27 for OS/2 Warp ARM64 (aarch64, musl): "tcc hello.c -o hello"
compiles and links a static program the system runs (at 0x60000000),
with its own assembler and linker. Needs musl-devel (headers, libc.a).

%prep
%autosetup -p1

%build
# tools for the configure runs: the cross toolchain as cc/ar, and the
# system's uname (configure reads the target OS from it)
mkdir -p xbin
ln -sf %{os2_cc} xbin/cc
ln -sf %{os2_llvm}/llvm-ar xbin/ar
ln -sf %{os2_llvm}/llvm-strip xbin/strip
printf '#!/bin/sh\necho Linux\n' > xbin/uname
chmod +x xbin/uname

# 1. a cross tcc for the Mac (arm64-tcc): it builds libtcc1.a (in a copy of
#    the tree: tcc 0.9.27 doesn't build out of its source directory)
mkdir -p cross
cp -R $(ls -A | grep -vx 'cross\|xbin') cross/
cd cross
./configure --config-musl > configure.log
make cross-arm64 CFLAGS="-O2 -DTCC_MUSL"
cd ..

# 2. the tcc that runs on the system
PATH="$PWD/xbin:$PATH" ./configure --cross-prefix="$PWD/xbin/" --cc=cc --ar=ar \
    --cpu=aarch64 --config-musl --prefix=/usr --libdir=/usr/lib --tccdir=/usr/lib/tcc \
    --sysincludepaths=/usr/lib/tcc/include:/usr/include \
    --libpaths=/usr/lib/tcc:/usr/lib --crtprefix=/usr/lib \
    --extra-cflags="-O2 -DCONFIG_TCC_OS2WARP %{os2_prefix_map}" > configure.log
PATH="$PWD/xbin:$PATH" make tcc

%install
install -d %{buildroot}%{_bindir} %{buildroot}/usr/lib/tcc/include %{buildroot}%{_mandir}/man1
install -m 755 tcc %{buildroot}%{_bindir}/tcc
install -m 644 include/*.h %{buildroot}/usr/lib/tcc/include/
install -m 644 cross/arm64-libtcc1.a %{buildroot}/usr/lib/tcc/libtcc1.a
[ -f tcc.1 ] && install -m 644 tcc.1 %{buildroot}%{_mandir}/man1/ || true
%{os2_strip}

%files
%license COPYING
%{_bindir}/tcc
/usr/lib/tcc

%changelog
* Fri Nov 13 2026 OS/2 Warp ARM64 build - 0.9.27-1.os2warp
- First package: tcc 0.9.27 for aarch64/musl, static programs at
  0x60000000; the relocations clang's libc.a needs; static GOT.
