# libusb fork

This package contains the 
[libusb](https://libusb.info) fork used by the
[Joulescope user-space driver](https://github.com/jetperch/joulescope_driver).
This fork is from the 
[libusb GitHub repo](https://github.com/libusb/libusb).
We applied unaccepted pull request
[#875](https://github.com/libusb/libusb/pull/875)
which allows fetching the serial descriptors without opening
the device.  This feature enables applications to discover
all connected Joulescopes, even ones that may be in use by 
other applications.

We also added cmake build support:
* CMakeLists.txt
* include/*/config.h for platform-specific config

We are including this libusb fork in the joulescope_driver project
to simplify the build and distribution project.  libusb remains
a separate project with the LGPL 2.1 license.
See [COPYING](COPYING) for details.  

The joulescope_driver is a separate work that uses and links to the 
libusb library.  The joulescope_driver uses the Apache 2.0 license.
Linking from this Apache 2.0 project to the LGPL 2.1 libusb 
project is allowed under the terms of the LGPL 2.1 license
provided that the copyrights remain intact and that we provide
a list of changes and the source code.  This directory
provides the source code and this file provides the list of 
changes.  Use diff against the libusb repo for a detailed
list of changes.
