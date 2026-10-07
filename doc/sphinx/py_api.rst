..
   SPDX-FileCopyrightText: Copyright 2022-2026 Jetperch LLC
   SPDX-License-Identifier: Apache-2.0

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

.. _py_api:

Python API
==========

.. toctree::
    :maxdepth: 3

    driver
    device_selection
    record
    program
    time64

Command-line tools
------------------

The package installs command-line entry points.  For the full list::

    python -m pyjoulescope_driver --help

The commands that operate on devices select them with ``--device`` (``-d``),
which accepts the same device specifications as
:meth:`Driver.find_devices`, such as ``-d js320`` or ``-d 8W2A``.
The commands that support any device also accept ``--brand``, such as
``--brand joulescope``.  For example::

    python -m pyjoulescope_driver info
    python -m pyjoulescope_driver values -d js320
    python -m pyjoulescope_driver metadata --brand joulescope

