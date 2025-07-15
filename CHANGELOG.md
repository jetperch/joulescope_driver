
# CHANGELOG

This file contains the list of changes made to the Joulescope driver.


## 1.10.0

2025 Jul 15

* Added Typescript support.  Thanks @biosbob !


## 1.9.4

2025 Jul 7

* Fixed tmap NULL pointer reference  (Joulescope UI 321).


## 1.9.3

2025 May 19

* Implemented "--set" option to record entry point.
* Changed "UTC add is not monotonically increasing" log errors to warnings 
  and reduced frequency.


## 1.9.2

2025 May 6

* Fixed self->tmap NULL pointer access in jsdrv_bufsig_info.
* Added NULL pointer handling to tmap.


## 1.9.1

2025 Apr 9

* Handle 500 ksps on-instrument downsampling by forcing to 250 ksps.
* Fixed time sync inaccuracy by including FPGA 1.3.3
  * handle large update gaps
  * bound error on sampling rate


## 1.9.0

2025 Mar 29

* Fixed TimeMap.get() in Python binding. 
* Deprecated jsdrv_buffer_info_s.time_map.  Use tmap.
* Renamed jsdrv_tmap_size to jsdrv_tmap_length.
* Changed jsdrv_tmap_get() signature.


## 1.8.0

2025 Mar 27

* Added improved time mapping to keep buffer time map consistent.
  * Added jsdrv/tmap.h, tmap.c and tmap_test.c.
  * Added jsdrv_buffer_info_s.tmap field.
  * Added jsdrv_buffer_info_s.decimate_factor field.
  * Maintained jsdrv_buffer_response_s.info.time_map functionality.
* Added Linux ARM64 builds.


## 1.7.4

2025 Mar 4

* Fixed memory leak on every JS220 command.
* Increased JSDRV_STREAM_DATA_SIZE from 64 kB to 256 kB to support
  larger buffer requests.


## 1.7.3

2025 Jan 7

* Fixed memory leak on JS220 repeated open without close.
* Fixed setup.py links (fixes PyPi links).


## 1.7.2

2024 Dec 10

* Fixed time sync inaccuracy by including FPGA 1.3.2


## 1.7.1

2024 Nov 25

* Fixed memory leak on error condition for Windows backend.
* Removed unused file device.h.
* Fixed time sync inaccuracy by including FPGA 1.3.1


## 1.7.0

2024 Nov 8

* Added support for JS220 on-instrument downsampling with fw 1.3.0 & fpga 1.3.0
* Migrated to numpy 2
* Added Python 3.13 support
* Dropped Python 3.9 support
* Added arbitrary scaler for current & voltage


## 1.6.0

2024 Oct 18

* Added support for notes to Record class and record entry point.
  * Added user_data parameter to Record.open().
  * Added "--note" option to record entry point.


## 1.5.6

2024 Sep 20

* Reduced log verbosity for some JS220 messages.
* Added js220_i128_is_neg.


## 1.5.5

2024 Aug 22

* Modified Python record entry point to use all connected Joulescopes by default.
  Use the "--serial_number" argument to record from a specific Joulescope
  when the host computer has more than one Joulescope connected.
* Catch exceptions on pyjoulescope_driver callbacks to Python code.  


## 1.5.4

2024 Jun 28

* Pinned numpy version dependency to < 2.


## 1.5.3

2024 May 28

* Improved libusb (macOS, linux) backend message processing.
  * Handle closed command queues to improve firmware update reliability.
  * Added device command queues to poll for improved performance.
  * Handle all pending device commands with each pass.


## 1.5.2

2024 May 8

* Added "capture" subcommand to jsdrv example executable.


## 1.5.1

2024 Apr 27

* Fixed JSDRV_DOWNSAMPLE_MODE_AVERAGE glitches due to C type conversion.
* Fixed incorrect downsampling initialization.


## 1.5.0

2024 Apr 24

* Added node.js binding.
* Fixed downsampling to round to nearest integer for integer types.
* Added downsampling mode JSDRV_DOWNSAMPLE_MODE_AVERAGE, but not yet connected.


## 1.4.10

2024 Mar 20

* Fixed JS110 to only issue one CTRL IN status request at a time.


## 1.4.9

2024 Mar 18

* Refactored record entry point.
* Fixed JS110 performance degradation (blocking status in device thread)  #8


## 1.4.8

2024 Feb 26

* Fixed python statistics entry point to remove 1.0 second default duration.


## 1.4.7

2024 Feb 14

* Initialized buffer_mgr_s instance_.context to NULL.
* Improved python binding error reporting.
* Added optional duration to statistics entry point.
* Bumped minimum pyjls version from 0.8.2 to 0.9.2.


## 1.4.6

2023 Dec 9

* Fixed timestamp on POSIX (macOS, linux) systems.


## 1.4.5

2023 Dec 8

* Fixed stream buffer use-after-free and remove timeout.
* Added fuzz tester.


## 1.4.4

2023 Dec 7

* Improved J110 & JS220 state reset on open.  Fixes JS110 fixes UTC time sync.
* Fixed JS220 communication reliability with FPGA 1.2.1 and FW 1.2.1.
* Improved JS110 time map for long-term stability.


## 1.4.1

2023 Nov 30

* Fixed JS220 "h/fs" restore.
* Improved JS220 UTC time sync with FW 1.2.0 & FPGA 1.2.0 support.
* Improved JS220 skip / drop sample handling.


## 1.4.0

2023 Nov 11

* Added jsdrv_calibration_hash and pyjoulescope_driver.calibration_hash.


## 1.3.21

2023 Nov 10

* Updated to FW 1.1.1.
* Added measure entry point to pyjoulescope_driver.


## 1.3.20

2023 Oct 26

* Updated to FPGA 1.1.0 for beta release (was mistakenly left at 1.0.4).


## 1.3.19

2023 Oct 25

* Fixed help text for "program" entry point.
* Build for Python 3.12.
* Upgraded to FW & FPGA 1.1.0 stable releases.
* Upgraded record entry point to pyjls 0.8.2 to 1.0.0.


## 1.3.18

2023 Jul 24

* Fixed buffer_signal summaryN incorrect when computing multiple values
  in a single call.
* Added "noexcept" to python callbacks. 
  Cython 3.0 deprecates implicit noexcept.


## 1.3.17

2023 Jul 11

* Fixed "in frame_id mismatch" warning log message on first frame.
* Fixed JS220 signal "s/X/ctrl" 0 not correctly closing signal.
* Added Record "auto" parameter to optionally bypass automatic signal enable/disable.


## 1.3.16

2023 Jun 14

* Parallelized GitHub Actions build.  Removed cibuildwheel.
* Fixed JS220 statistics for macOS.  1.3.15 was segfaulting on unaligned accesses.
* Added args parameter to pyjoulescope_driver.__main__.run().
* Added quit_ handling to jsdrv.exe examples.
* Bumped pyjls version check from 0.7.0 to 0.7.2.


## 1.3.15

2023 Jun 8

* Fixed JS220 firmware images omitted by 1.3.14 build process changes.
* Changed firmware image download to script invoke, not import.


## 1.3.14

2023 Jun 8

* Improved documentation.
* Improved GitHub Actions build process.
* Moved test/jsdrv_util to example/jsdrv.
* Bumped minimum python version from 3.8 to 3.9.


## 1.3.12

2023 May 31

* Reduced libusb backend log level.  Was too active for JS110 statistics.
* Bumped JLS version to 0.7.0.


## 1.3.11

2023 May 24

* Added JS110 on-instrument (sensor) statistics option #3.


## 1.3.10

2023 May 19

* Improved threading and priorities on Windows.
* Improved Windows timer resolution (timeBeginPeriod).


## 1.3.9

2023 May 17

* Fixed installation on Ubuntu #6.


## 1.3.8

2023 May 16

* Updated info entry point.
* Added automatic include path for jsdrv static CMake builds.
* Added support for building a shared library.
  Initialize build subdir with "cmake -DBUILD_SHARED_LIBS=ON .."


## 1.3.7

2023 Apr 28

* Fixed "info" entry point to correctly display multiple Joulescopes.
* Fixed "record" entry point to correctly add first UTC timestamp.
* Cleaned up build & install process (needs more work).


## 1.3.6

2023 Apr 27

* Fixed JS110 sample_id for downsampled 1-bit channels.
* Updated to pyjls 0.6.1 for improved robustness.


## 1.3.5

2023 Apr 26

* Added JS220 FW 1.0.7 and FPGA 1.0.4 as alpha & beta.


## 1.3.4

2023 Apr 26

* Fixed record jls version check.
* Send buffer signal clear on free.
* Updated to pyjls 0.6.0.
  * Fixed pyjoulescope_driver.record to not remove sample_id offset.  


## 1.3.3

2023 Apr 19

* Cleared all message fields at allocation.
* Added api_timeout entry point test.
* Improved thread entry point test.
* Reordered unsubscribe to ensure callback validity.
* Added malloc/free mutex for guaranteed thread safety.
* Added runtime pyjls version check.
* Improved logging robustness and thread safety.
* Fixed JS110 open causing IN+ to OUT+ disconnect.
* Added JS110 open modes: defaults, resume.


## 1.3.2

2023 Apr 13

* Improved record close error handling.
* Added JS220 streaming data ignore when device not open.
* Improved record entry point.
  * Open in "defaults" mode by default with optional "restore".
  * Added parameter "--set" option.


## 1.3.1

2023 Apr 4

* Decreased JS110 status polling interval to reduce USB message spamming.
* Added JS110 streaming when only statistics requested (uses host-side stats).
* Increased process priority and backend thread priority for Windows.


## 1.3.0

2023 Mar 30

* Added pyjoulescope_driver.time64 module (from UI).
* Fixed buffer_signal summary_get handling on zero size.
* Added "record" module and entry point to record streaming samples.
* Fixed buffer_signal range advertisement when empty.
* Improved skipped / duplicate sample handling for JS220.


## 1.2.2

2023 Mar 19

* Fixed intermittent timeout broken for API calls.


## 1.2.1

2023 Mar 16

* Truncate memory buffer sample responses that are too long (segfault).
* Fixed buffer signal shift correction overflowing buffer (segfault).
* Fixed garbage data at end when shift required for u1 and u4 data types.
* Fixed zero length message send for highly downsampled signals.


## 1.2.0

2023 Mar 9

* Added memory buffer for f32, u4, u1 data types.
* Bumped python support (3.8 - 3.11).  Dropped 3.7.
* Added API struct jsdrv_time_map_s and functions
  jsdrv_time_from_counter(), jsdrv_time_to_counter.
* Added jsdrv_time_map_s to 
  jsdrv_stream_signal_s and jsdrv_statistics_s.
* Added host-side time map.
* Fixed JS110 sample stream message size.
* Fixed buffer_signal sample and utc response time entries. 


## 1.1.4

2023 Jan 25

* Added JS110 GPI read request: s/gpi/+/!req -> s/gpi/+/!value.
* Fixed incorrect topic match on {device}/@/!finalize


## 1.1.3

2023 Jan 24

* Fixed open to correctly handle error on lower-level device open.
* Improved statistics output to include time/sample_freq and time/range.
* Reduced DEVICES_MAX for libusb backend to prevent breaking select.


## 1.1.2

2022 Dec 20

* Fixed i128 math functions js220_i128_neg() and js220_i128_lshift().
* Fixed incorrect constant for platforms with i32 constants.


## 1.1.1

2022 Nov 30

* Fixed build dependencies.
* Specified python 3.10 for GitHub actions.


## 1.1.0

2022 Nov 11

* Modified jsdrv_stream_signal_s (requires app recompile).
  * Fixes pyjoulescope and pyjoulescope_ui integration. 
  * Added sample_rate. 
  * Added decimate_factor.
* Added JS220 host-side downsampling.
* Rename JS110 topic s/fs to h/fs, matches new JS220 topic. 
* Fixed old JS220 data sent on stream enable after disable.
* Added free for partial input stream messages on device close.
* Fixed firmware image include and added pkgdata load check.


## 1.0.7

2022 Nov 8

* Improved documentation.


## 1.0.6

2022 Nov 2

* Fixed build from tar.gz package.
* Updated README.


## 1.0.5

2022 Nov 1

* Fixed JS110 current range processing for window N and M.
* Fixed JS110 sample alignment.
* Fixed JS110 statistics generation time and rate.


## 1.0.4

2022 Oct 30

* Added JS220_CTRL_OP_DISCONNECT for cleaner disconnect/reconnect.
  Requires js220_ctrl firmware 1.0.4, no effect on earlier versions.
* Fixed JS110 support.
* Improve firmware update support.


## 1.0.3

2022 Oct 24

* Fixed linux by disabling BULK IN timeout.
  Linux was losing data on timeout.
  Fixed cancel, which still allows graceful shutdown.
* Added JS110 downsampling on host. 
* Switched from obsolete pypiwin32 to pywin32.
* Fixed js110_sp_process when i_range was off or missing. 


## 1.0.2

2022 Oct 15

* Fixed jsdrv_util app_match to work with multiple Joulescopes.


## 1.0.1

2022 Oct 9

* Added native and python threading demonstrations.
* Improve python binding to support u64 and i64 types.
* Added firmware/gateware updates. 
  * Added build support for including most recent firmware images.
  * Added "program" entry point.
* Fixed access violation on publish to close device.
* Fixed python Driver.device_paths returning [""] instead of []
  when no devices found.
* Reduced bulk in error on device removal to warning.


## 1.0.0

2022 Oct 6

* Fixed JS220 power computation.
* Fixed trigger GPI spec (7 not 255).
* Fixed sample_id 32-bit rollover handling.
* Added publish bytes support (needed for memory writes).


## 0.2.7

2022 Oct 4

* Added host-side power computation to JS220.
* Improved JS110 driver to provide current_range, gpi0, gpi1.
* Fixed power, charge and energy computation.
  Power fixes require FPGA 0.2.6 upgrade.
* Fixed JS110 stop/start streaming.
* Added JS110 statistics.


## 0.2.6

2022 Sep 29

* Reduced POSIX file descriptor limit request to 4096 max.


## 0.2.5

2022 Sep 26

* Fixed process hang due to logging on macOS & Linux.
* Fixed device disconnect for macOS & Linux.
* Increased file handle limit on macOS & Linux.


## 0.2.4

2022 Sep 24

* Added responder for removed devices.
* Fixed JSDRV_MSG_DEVICE_ADD retained subscribe.
  Was sending each device then full comma-separated list.
* Added check for API call from jsdrv frontend thread. 


## 0.2.3

2022 Sep 21

* Fixed support for multiple (not just first found) of same device type.
* Improved JS110 support.
* Added macOS and Linux support using libusb library.


## 0.2.2

2022 Sep 9

* Completed JS220 memory read operation.
* Completed JS220 memory erase operation.
* Modifed jsdrvp_msg_alloc_value to support heap allocated values, not
  just jsdrv_publish().  Needed for read data messages.
* Added jsdrv_topic_remove().
* Completed controller-side signed & encrypted & signed firmware update.
* Move UI to private repository.  Need to avoid Qt dependencies here.
* Removed "noise.py" entry point, which is not meaningful to end users.
* Added python command-line --log_level and --jsdrv_log_level options.
* Fixed python publish string values encoding.
* Improved native metadata validating to accept integer values as strings.
* Renamed python "set.py" to "set_parameter.py" to avoid reserved word "set".
* Added JS220 "opening" state to "h/state" metadata.
* Simplified native jsdrv_util set to use native support for string values. 
* Fixed driver based upon beta build feedback.
* Fixed JS220 handling of missing/duplicated sample ids.
* Converted to element_size_bits from element_size_pow2.
* Added GPIO streaming support (u1 data type).
* Redefined JSDRV_DATA_TYPE_FLOAT to align with JLS v2.
* Reduced logging verbosity.


## 0.2.1

2022 Aug 1

* Initial public commit.
