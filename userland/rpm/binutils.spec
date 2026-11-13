# binutils for UNIXROOT: built by rpmbuild from the GNU release
# (userland/build-rpms.sh binutils), with the system's cross toolchain
# (macros.os2warp). Installs under /usr, which the system maps onto
# UNIXROOT's drive (D:).
Name:           binutils
Version:        2.44
Release:        2.os2warp
Summary:        GNU assembler, linker and binary tools (as, ld, objdump, ...)
License:        GPL-3.0-or-later
URL:            https://www.gnu.org/software/binutils/
Source0:        https://ftp.gnu.org/gnu/binutils/binutils-%{version}.tar.xz
Patch0:         binutils-2.44-os2warp-ld-base.patch
BuildArch:      aarch64
AutoReqProv:    no

%description
The GNU binary utilities for OS/2 Warp ARM64: as (assembler), ld
(linker), ar, ranlib, nm, objdump, objcopy, readelf, strip, size,
strings, addr2line, c++filt, elfedit and gprof, as static AArch64
programs. ld links programs at 0x60000000, where the system runs them.

%prep
%autosetup -p1

%build
# native (aarch64-linux-musl for aarch64-linux-musl); no gold or gprofng
# (C++), no plugins (no dlopen), the bundled zlib
%{os2_configure} --target=aarch64-linux-musl \
    --disable-werror --disable-gdb --disable-gdbserver --disable-sim \
    --disable-gold --disable-gprofng --disable-plugins --disable-shared \
    --enable-static --without-zstd --without-debuginfod --with-system-zlib=no \
    --disable-libctf
make -j%{os2_jobs} MAKEINFO=true

%install
make install DESTDIR=%{buildroot} MAKEINFO=true
# the programs and man pages only: not the libraries for binutils-using
# tools, nor the second copies Linux would hard-link (no hard links here)
rm -rf %{buildroot}/usr/include %{buildroot}/usr/lib %{buildroot}/usr/share/info \
       %{buildroot}/usr/aarch64-linux-musl %{buildroot}/usr/bin/ld.bfd
%{os2_strip}

%files
%license COPYING COPYING3
%{_bindir}/*
%{_mandir}/man1/*

%changelog
* Fri Nov 13 2026 OS/2 Warp ARM64 build - 2.44-2.os2warp
- Built by rpmbuild from the release tarball (%%prep, %%build, %%install);
  the ld load address as a patch.
* Fri Nov 13 2026 OS/2 Warp ARM64 build - 2.44-1.os2warp
- First package: binutils 2.44 for UNIXROOT, built with the musl toolchain.
