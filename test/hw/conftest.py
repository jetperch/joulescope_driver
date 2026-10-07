# SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# The scripts in test/hw/ are standalone hardware-in-the-loop tools that
# require a physically connected device.  Their test_* functions take the
# device path as a parameter (supplied by each script's main()), which
# pytest would misread as a missing fixture.  Keep them out of pytest
# collection; run them directly, e.g.:
#     JSDRV_HW_DEVICE=u/js320/X2VJ python test/hw/test_open_state.py
collect_ignore_glob = ['test_*.py']
