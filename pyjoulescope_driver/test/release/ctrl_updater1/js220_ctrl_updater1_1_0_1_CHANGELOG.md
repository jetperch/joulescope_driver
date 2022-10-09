# Changelog

This file describes the history of releases and their associated changes
for the Joulescope JS220 controller firmware.


## 1.0.1

2022 Oct 7

* Reduced coms response ACK/NACK latency.


## 1.0.0

2022 Oct 6

* Fixed duplicate USB Bulk IN messages.
* Added ZLP retry when USB FIFO IN still busy.
* Improved sensor->host overflow handling due to host USB inactivity.
* Added "c/target" read-only topic to simplify firmware updates.
* Improved flash write robustness - DSB and retry.


## 0.1.2

2022 Sep 15

* Disabled 144 MHz trace clock.
* Disabled PIN_DEBUGx pins.
* Changed HWID to pull down, only pull up when needed to read value.
* Created SENSOR_PWR_EN in addition to SENSOR_PWR_EN_L.


## 0.1.1

2022 Sep 11

* Improved error recovery for ESD events.


## 0.1.0

2022 Sep 5

* Updated controller LED.
* Added sensor PWM boost output & ADC feedback controller.
* Fixed flash write - added memory barrier and blocking.
* Fixed USB to pass chapter 9 tests.


## 0.0.5

2022 Aug 11

* Added write ack.
* Completed unencrypted memory read/erase/write.
* Completed controller-side signed & encrypted & signed firmware update.


## 0.0.3

2022 July 19

* Added bootloader.
* Moved application to offset 512 kB in flash.
* Created updaters.
* Updated memory map to move personalities.
* Added port3 API for memory read/write/erase & firmware update.


## 0.0.1

2021 Aug 19

* Initial commit.
