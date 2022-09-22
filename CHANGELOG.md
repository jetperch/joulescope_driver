
# CHANGELOG

This file contains the list of changes made to the Joulescope driver.

## 0.2.4

2022 Sep 22 [in progress]

* Added responder for removed devices.


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
