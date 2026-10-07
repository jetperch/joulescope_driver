# SPDX-FileCopyrightText: Copyright 2026 Jetperch LLC
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""A device path string that parses its own fields.

Device paths have the form "{backend}/{model}/{serial_number}", such as
"u/js320/31NB".  Driver.device_paths() returns DevicePath instances.
Paths from other sources, such as "@/!add" callbacks, command-line
arguments or configuration files, are plain str.  Use DevicePath(path)
to convert them.
"""


#: The lowercase models for each brand.
BRANDS_TO_MODELS = {
    'Joulescope': ('js110', 'js220', 'js320'),
}

#: Case-insensitive alternate names for brands in BRANDS_TO_MODELS.
BRAND_ALIASES = {
    'js': 'Joulescope',
}

_MODEL_TO_BRAND = {model: brand for brand, models in BRANDS_TO_MODELS.items() for model in models}
_BRAND_LOOKUP = {brand.lower(): brand for brand in BRANDS_TO_MODELS}
_BRAND_ALIAS_LOOKUP = {alias.lower(): brand for alias, brand in BRAND_ALIASES.items()}


def brand_validate(brand):
    """Validate a brand name.

    :param brand: The case-insensitive brand name or alias, such as
        "joulescope" or "js".
    :return: The brand name, such as "Joulescope".
    :raise TypeError: If brand is not a str.
    :raise ValueError: If brand is not supported.
    """
    if not isinstance(brand, str):
        raise TypeError(f'brand must be str, not {type(brand).__name__}')
    key = brand.lower()
    key = _BRAND_ALIAS_LOOKUP.get(key, key).lower()
    try:
        return _BRAND_LOOKUP[key]
    except KeyError:
        expect = ', '.join([*BRANDS_TO_MODELS, *BRAND_ALIASES])
        raise ValueError(f'Unsupported brand "{brand}", expected one of: {expect}') from None


class DevicePath(str):
    """A device path str, such as "u/js320/31NB".

    DevicePath is a str subclass, so it works anywhere a device path
    string does.  The field properties return None for paths that do
    not have the "{backend}/{model}/{serial_number}" form.
    """

    __slots__ = ()

    def _parts(self):
        parts = self.split('/')
        return parts if len(parts) == 3 else None

    @property
    def backend(self):
        """The backend, such as "u" for USB, or None."""
        parts = self._parts()
        return None if parts is None else parts[0]

    @property
    def model(self):
        """The lowercase model, such as "js320", or None."""
        parts = self._parts()
        return None if parts is None else parts[1].lower()

    @property
    def serial_number(self):
        """The serial number with its original case, such as "31NB", or None."""
        parts = self._parts()
        return None if parts is None else parts[2]

    @property
    def brand(self):
        """The brand, such as "Joulescope", or None if unknown."""
        return _MODEL_TO_BRAND.get(self.model)

    def match(self, spec):
        """Check if this path matches a user-provided device specification.

        :param spec: The device specification, which is one of:

            * the full device path, such as "u/js320/31NB".
            * the model and serial number, such as "js320/31NB".
            * the model and serial number, such as "js320-31NB".
            * the model, such as "js320".
            * the serial number, such as "31NB".

            Matching is case-insensitive, and serial numbers must match exactly.
        :return: True on a match, False otherwise.
        """
        parts = self._parts()
        if parts is None or spec is None:
            return False
        spec = spec.lower().strip('/')
        _, model, serial_number = [p.lower() for p in parts]
        return spec in (self.lower(), model, f'{model}/{serial_number}',
                        f'{model}-{serial_number}', serial_number)
