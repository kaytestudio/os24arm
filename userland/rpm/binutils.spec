# binutils for UNIXROOT - built by userland/build-binutils.sh, packaged by
# userland/build-rpms.sh (rpmbuild on the Mac). Installs under /usr, which
# the system maps onto UNIXROOT's drive (D:).
Name:           binutils
Version:        2.44
Release:        1.os2warp
Summary:        GNU assembler, linker and binary tools (as, ld, objdump, ...)
License:        GPL-3.0-or-later
URL:            https://www.gnu.org/software/binutils/
BuildArch:      aarch64
AutoReqProv:    no

%description
The GNU binary utilities for OS/2 Warp ARM64: as (assembler), ld
(linker), ar, ranlib, nm, objdump, objcopy, readelf, strip, size,
strings, addr2line, c++filt and elfedit, as static AArch64 programs.

%install
mkdir -p %{buildroot}
cp -R %{pkgroot}/. %{buildroot}/

%files
/usr/bin/*
/usr/share/man/man1/*

%changelog
* Fri Nov 13 2026 OS/2 Warp ARM64 build - 2.44-1.os2warp
- First package: binutils 2.44 for UNIXROOT, built with the musl toolchain.
