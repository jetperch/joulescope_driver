# Move the live device tests out of the Python unit tests

`pyjoulescope_driver/test/test_driver_context.py::TestLiveDevice` opens the
first connected Joulescope whenever one is connected.  The unit tests
therefore depend on the bench: when another program holds that instrument,
the four tests fail with `IN_USE`, and they briefly claim an instrument that
a long-running test may be using.  `test/hw/` holds the tests that need a
device.

Found 2026 Oct 9, while a soak test held every instrument.  Before then, a
failed open in `test_finalize_closes_opened` also leaked its Driver, so the
rest of the run failed with `jsdrv_initialize failed 21 IN_USE` (only one
Driver per process).  That leak is fixed with `addCleanup(d.finalize)`.

## Plan

1. Move `TestLiveDevice` to `test/hw/`, or skip it unless an environment
   variable, such as `JSDRV_TEST_DEVICE`, names the instrument to use.
2. Keep the device-free tests of the same behavior, with `ListDriver` and
   `CloseRecordDriver`, in the unit tests.
