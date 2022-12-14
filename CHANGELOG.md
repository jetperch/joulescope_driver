
# CHANGELOG

This file contains the list of changes made to the Joulescope driver.


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
