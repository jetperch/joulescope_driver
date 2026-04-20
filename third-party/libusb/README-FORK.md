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

We also backported the macOS fix from upstream commit
[d66ffcd](https://github.com/libusb/libusb/commit/d66ffcd)
("darwin: fix potential crash at darwin_exit") to
`libusb/os/darwin_usb.c`.  When `darwin_first_time_init()` fails
before the async event thread starts (for example, when a cached
device reference is leaked across a `libusb_init`/`libusb_exit`
cycle), the subsequent `darwin_exit()` cleanup would dereference a
NULL `CFRunLoopSourceRef` and segfault, and would also
`pthread_join` a thread that was never created.  The backport adds a
`libusb_darwin_at_started` flag (set under `libusb_darwin_at_mutex`
after `pthread_create` succeeds) and guards both the runloop
shutdown signal and the join against those uninitialized states.

We also made `darwin_cleanup_devices()` in `libusb/os/darwin_usb.c`
resilient to leaked cached-device references.  A
`libusb_init`/`libusb_exit` cycle on macOS with a device still
physically attached leaves a residual reference on the
`darwin_cached_devices` list (libusb's `usbi_hotplug_exit()` does not
drop the `usb_devs` list reference for devices whose refcount is
greater than one, and `libusb_exit()` deliberately warns but does not
force-free such `libusb_device` instances).  The upstream cleanup
decrements once and leaves the entry on the list; the next
`libusb_init()` then fails with `LIBUSB_ERROR_OTHER` because
`darwin_first_time_init()` rejects a non-empty cache.  We now
force-remove such leaked entries from the cache list (and reinitialize
their list link as self-referential so the eventual `list_del` from
`darwin_deref_cached_device()` is a safe no-op if the lingering
`libusb_device` is later destroyed).  This lets re-initialization
succeed across the finalize/initialize cycles that the Joulescope
driver fuzz test exercises.

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
