Name:           libouistiti
Version:        3.1
Release:        2
Summary:        HTTP server
Group:          Library/Network
License:        MIT
URL:            https://github.com/ouistiti-project/libhttpserver
Vendor:         Object Computing, Incorporated
Source:         https://github.com/mchalain/libhttpserver/archive/refs/tags/libouistiti-%{version}.tar.gz
Prefix:         %{_prefix}
Packager:       Marc Chalain <marc.chalain@gmail.com>
BuildRoot:      %{_tmppath}/libhttpserver
Requires:	mbedtls
BuildRequires:	mbedtls-devel


%description
HTTP server library. libouistiti manage HTTP connection

%package devel
Version:        %{version}
Group:          Development/Tools
Summary:        Development tools for libouistiti
Requires:       libouistiti

%description devel
Development files for libouistiti

%global debug_package %{nil}

%prep
%setup -q -c libouistiti

%build
CFLAGS="$RPM_OPT_FLAGS" make package=ouistiti prefix=/usr libdir=/usr/lib64/ STATIC=n defconfig
CFLAGS="$RPM_OPT_FLAGS" make package=ouistiti

%install
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf $RPM_BUILD_ROOT
CFLAGS="$RPM_OPT_FLAGS" make package=ouistiti DESTDIR=$RPM_BUILD_ROOT install

%clean
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf $RPM_BUILD_ROOT
CFLAGS="$RPM_OPT_FLAGS" make package=ouistiti clean

%files
%defattr(-,root,root)
%doc README.md LICENSE
%{_libdir}/liboui*.so
%{_libdir}/liboui*.so.%{version}

%files devel
%{_includedir}/*
%{_libdir}/pkgconfig/ouistiti.pc

%changelog


