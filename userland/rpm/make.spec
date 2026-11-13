# GNU make for UNIXROOT: built by rpmbuild from the GNU release
# (userland/build-rpms.sh make), with the system's cross toolchain
# (macros.os2warp). Installs /usr/bin/make on UNIXROOT's drive (D:).
Name:           make
Version:        4.4.1
Release:        1.os2warp
Summary:        GNU make: builds programs from their Makefiles
License:        GPL-3.0-or-later
URL:            https://www.gnu.org/software/make/
Source0:        https://ftp.gnu.org/gnu/make/make-%{version}.tar.gz
BuildArch:      aarch64
AutoReqProv:    no

%description
GNU make for OS/2 Warp ARM64, a static AArch64 program: it reads a
Makefile and runs the commands that bring its targets up to date, with
the system's sh (busybox) running the recipes. No "load" directive (no
dynamic loading here) and no Guile.

%prep
%autosetup

%build
%{os2_configure} --disable-load --without-guile --disable-dependency-tracking
make -j%{os2_jobs} MAKEINFO=true

%install
make install DESTDIR=%{buildroot} MAKEINFO=true
rm -rf %{buildroot}%{_infodir} %{buildroot}%{_includedir}
%{os2_strip}

%files
%license COPYING
%doc README NEWS
%{_bindir}/make
%{_mandir}/man1/make.1*

%changelog
* Fri Nov 13 2026 OS/2 Warp ARM64 build - 4.4.1-1.os2warp
- First package: GNU make 4.4.1 for UNIXROOT.
