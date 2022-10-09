
# Joulescope sensor FPGA


## Version 1.0.1

2022 Oct 8

* Added TX spacing every 4 frames to terminate back-to-back transmission.
  The host controller only receives data when CS_N goes high.
* Decreased TX timeout.
* Added hardware transmit frame id assignment and overflow handling.


## Version 1.0.0

2022 Oct 6

* Released to production.

Planned features not yet implemented include:

* UTC time sync
* Downsampling
* Soft-fuse
* Triggering
* UART RX & TX


## Version 0.2.6

2022 Oct 4

* Fixed statistics power x1, now same precision as x2.


## Version 0.2.5

2022 Sep 11

* Fixed cal delay - out_data_valid goes active on adc_data_valid.
* Fixed si_fwd sample_id latch.  Always latch on current_valid.
* Synchronized sample_id to adc_data_valid.
