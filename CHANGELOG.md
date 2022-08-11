
# CHANGELOG

This file contains the list of changes made to the Joulescope driver.


## 0.2.2

2022 Aug 11 [in progress]

* Completed JS220 memory read operation.
* Completed JS220 memory erase operation.
* Modifed jsdrvp_msg_alloc_value to support heap allocated values, not
  just jsdrv_publish().  Needed for read data messages.
* Added jsdrv_topic_remove().
* Completed controller-side signed & encrypted & signed firmware update.


## 0.2.1

2022 Aug 1

* Initial public commit.
