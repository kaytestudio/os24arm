# musl-devel for UNIXROOT: the C library to compile and link against on the
# system - headers, libc.a and the startup files - built by rpmbuild from
# the musl release (userland/build-rpms.sh musl-devel). Static only: the
# kernel runs static programs.
Name:           musl-devel
Version:        1.2.5
Release:        1.os2warp
Summary:        The musl C library: headers, libc.a and startup files
License:        MIT
URL:            https://musl.libc.org/
Source0:        https://musl.libc.org/releases/musl-%{version}.tar.gz
BuildArch:      aarch64
AutoReqProv:    no

%description
musl 1.2.5 for OS/2 Warp ARM64 (aarch64): /usr/include, /usr/lib/libc.a
(libm, libpthread, ... are in it) and crt1.o, crti.o, crtn.o - what a C
compiler on the system (tcc) needs to build static programs.

%prep
%autosetup -n musl-%{version}

%build
CC="%{os2_llvm}/clang --target=aarch64-linux-musl" \
AR="%{os2_llvm}/llvm-ar" RANLIB="%{os2_llvm}/llvm-ranlib" \
CFLAGS="-O2 -fno-stack-protector -mno-outline-atomics" LDFLAGS="-fuse-ld=lld" \
    ./configure --target=aarch64-linux-musl --prefix=/usr --syslibdir=/lib \
                --disable-shared --enable-static
make -j%{os2_jobs}

%install
make install DESTDIR=%{buildroot}

%files
%license COPYRIGHT
%{_includedir}/*
%{_libdir}/*.a
%{_libdir}/*.o

%changelog
* Fri Nov 13 2026 OS/2 Warp ARM64 build - 1.2.5-1.os2warp
- First package: musl 1.2.5 headers and static libraries for UNIXROOT.
