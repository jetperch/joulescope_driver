.. _h_meta:

Metadata handling
=================

.. doxygengroup:: jsdrv_meta
    :members:


Host-side metadata
------------------

The driver defines its own topic metadata, such as JS320 ``h/fs`` and the
memory buffer ``m/`` topics, in ``jsdrvp_param_s`` tables in
``src/**/*_params.c``.  Tables in ``src/devices/<model>/`` apply to that
device model.  Other tables are global driver topics.  Topics may contain
``{buf}`` and ``{sig}`` placeholders for the buffer and signal ids.

``pyjoulescope_driver.metadata_extract`` parses these tables into the
packaged ``host_params.json``.  After editing a table, regenerate it with::

    python -m pyjoulescope_driver.metadata_extract --write

A unit test fails when ``host_params.json`` is stale.

The ``metadata`` entry point combines this host-side metadata with the
firmware build's ``pubsub_metadata.json`` files to document all topics
without a device::

    python -m pyjoulescope_driver metadata --firmware js320.zip --out js320.html

With a connected device, ``--diff`` compares the live metadata against a
saved JSON file to verify the offline output::

    python -m pyjoulescope_driver metadata --diff js320.json
