%define udev_dir %{_prefix}/lib/udev

Name:           libinput
Version:        1.8.0
Release:        0
License:        MIT
Summary:        Input devices for display servers and other applications
Url:            http://www.freedesktop.org/software/libinput/libinput-%{version}.tar.xz
Group:          System/Libraries
Source:         %{name}-%{version}.tar.gz
Source1001:		%name.manifest
#X-Vcs-Url:      git://anongit.freedesktop.org/wayland/libinput

BuildRequires:  make
BuildRequires:  pkgconfig(libevdev)
BuildRequires:  pkgconfig(libevent)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(mtdev)
BuildRequires:  pkgconfig(ttrace)

%global TZ_SYS_RO_SHARE  %{?TZ_SYS_RO_SHARE:%TZ_SYS_RO_SHARE}%{!?TZ_SYS_RO_SHARE:/usr/share}

%description

libinput is a library that handles input devices for display servers and
other applications that need to directly deal with input devices.

It provides device detection, device handling, input device event
processing and abstraction so minimize the amount of custom input
code the user of libinput need to provide the common set of
functionality that users expect.


%package devel
Summary:    Input devices for display servers and other applications
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description devel

libinput is a library that handles input devices for display servers and
other applications that need to directly deal with input devices.

It provides device detection, device handling, input device event
processing and abstraction so minimize the amount of custom input
code the user of libinput need to provide the common set of
functionality that users expect.

%prep
%setup -q
cp %{SOURCE1001} .

export LIBINPUT_EXECUTABLE_CFLAGS=" -fPIE "
export LIBINPUT_EXECUTABLE_LIBS=" -pie "

%autogen --with-udev-dir=%{udev_dir} --disable-documentation --disable-debug-gui --disable-tests --disable-libwacom

%build
%__make %{?_smp_mflags}

%install
%make_install

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig


%files
%manifest %{name}.manifest
%defattr(-,root,root)
%license COPYING
%{_libdir}/*.so.*

%files devel
%manifest %{name}.manifest
%defattr(-,root,root,-)
%{_includedir}/*
%{_libdir}/*.so
%{_libdir}/pkgconfig/*
