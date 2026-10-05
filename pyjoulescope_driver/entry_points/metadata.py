# Copyright 2026 Jetperch LLC
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

from pyjoulescope_driver import Driver, __version__
from pyjoulescope_driver.metadata_extract import host_metadata
import html
import json
import os
import zipfile


_FORMATS = ['json', 'yaml', 'html']
_EXTENSIONS = {
    '.json': 'json',
    '.yaml': 'yaml',
    '.yml': 'yaml',
    '.html': 'html',
    '.htm': 'html',
}
_COLUMN_ORDER = ['dtype', 'brief', 'detail', 'default', 'options', 'range', 'format', 'flags']
_COLUMNS_HIDDEN = ['detail']
_FIRMWARE_METADATA_FILENAME = 'pubsub_metadata.json'
_DEVICE_FLAGS = ['ro', 'hide', 'dev']  # the flags meta_binary.c publishes
_NON_NUMERIC_DTYPES = ['str', 'json', 'bin', 'std', 'stdmsg', 'frm', 'frame']


def parser_config(p):
    """Display device metadata."""
    p.add_argument('--format',
                   type=str.lower,
                   choices=_FORMATS,
                   help='The output format.  When omitted, infer from the --out '
                        + 'file extension, defaulting to json.')
    p.add_argument('--device',
                   help='The target device path.  Optional when only one device is connected.')
    p.add_argument('--out',
                   help='The output file path.  When omitted, write to stdout.')
    p.add_argument('--open', '-o',
                   choices=['defaults', 'restore'],
                   default='restore',
                   help='The device open mode.  Defaults to "restore".')
    p.add_argument('--firmware',
                   nargs='+',
                   help='Generate offline, without a device, from firmware build '
                        + f'{_FIRMWARE_METADATA_FILENAME} files or firmware zip files, '
                        + 'combined with the host-side metadata for --model.')
    p.add_argument('--model',
                   default='js320',
                   help='The device model for --firmware host-side metadata.  Defaults to js320.')
    p.add_argument('--title',
                   help='The HTML title.')
    p.add_argument('--diff',
                   help='Compare against this JSON metadata file, instead of writing output.  '
                        + 'Returns 1 when different.')
    return on_cmd


def format_resolve(fmt, out_path):
    """Resolve the output format.

    :param fmt: The explicit format or None.
    :param out_path: The output file path or None.
    :return: One of 'json', 'yaml', 'html'.
    """
    if fmt is not None:
        return fmt
    if out_path is not None:
        ext = os.path.splitext(out_path)[1].lower()
        if ext in _EXTENSIONS:
            return _EXTENSIONS[ext]
    return 'json'


def device_select(device_paths, device):
    """Select the target device.

    :param device_paths: The list of connected device paths.
    :param device: The requested device path or None.
    :return: The selected device path.
    :raise ValueError: If the selection is empty or ambiguous.
    """
    if not device_paths:
        raise ValueError('No connected Joulescopes found')
    if device is None:
        if len(device_paths) == 1:
            return device_paths[0]
        raise ValueError('Multiple devices found, specify one with --device:\n  '
                         + '\n  '.join(device_paths))
    if device not in device_paths:
        raise ValueError(f'Device {device} not found.  Connected devices:\n  '
                         + '\n  '.join(device_paths))
    return device


def metadata_load(driver, device_path):
    """Load the retained metadata snapshot for an open device.

    :param driver: The active Driver instance.
    :param device_path: The open device path.
    :return: The dict mapping device-relative subtopic to metadata dict.
    """
    meta = {}
    prefix_len = len(device_path) + 1

    def on_metadata(topic, value):
        if topic[-1] == '$':
            topic = topic[:-1]
        meta[topic[prefix_len:]] = value

    driver.subscribe(device_path, 'metadata_rsp_retain', on_metadata)
    driver.unsubscribe(device_path, on_metadata)
    return meta


def firmware_meta_to_device(doc):
    """Convert a firmware pubsub_metadata.json document to device form.

    Matches the device metadata the driver publishes on open: see
    meta_fetch_on_topic() in mb_device.c and meta_binary.c.

    :param doc: The parsed pubsub_metadata.json document.
    :return: The dict mapping device-relative topic to metadata dict.
    """
    prefix = doc['project']['prefix']
    meta = {}
    for topic, value in doc['topics'].items():
        if topic.startswith('./'):
            topic = prefix + topic[1:]
        value = dict(value)
        default = value.get('default')
        if isinstance(default, str) and value.get('dtype') not in _NON_NUMERIC_DTYPES:
            # unresolved build symbol: the binary blob encodes 0 (pyminibitty _encode_value)
            try:
                float(default)
            except ValueError:
                value['default'] = 0
        if 'options' in value:  # the binary blob stores aliases as strings
            value['options'] = [o[:2] + [str(a) for a in o[2:]] for o in value['options']]
        flags = value.pop('flags', None) or []
        if isinstance(flags, str):
            flags = [flags]
        flags = [f for f in _DEVICE_FLAGS if f in flags]
        if flags:
            value['flags'] = flags
        meta[topic] = value
    return meta


def firmware_load(paths):
    """Load device metadata from firmware build outputs.

    :param paths: The list of pubsub_metadata.json or firmware zip paths.
        Zip files contribute every contained pubsub_metadata.json.
    :return: The dict mapping device-relative topic to metadata dict.
    """
    docs = []
    for path in paths:
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as z:
                names = sorted(n for n in z.namelist()
                               if n.split('/')[-1] == _FIRMWARE_METADATA_FILENAME)
                if not names:
                    raise ValueError(f'{path}: contains no {_FIRMWARE_METADATA_FILENAME}')
                docs.extend(json.loads(z.read(n)) for n in names)
        else:
            with open(path, 'r', encoding='utf-8') as f:
                docs.append(json.load(f))
    meta = {}
    for doc in docs:
        meta.update(firmware_meta_to_device(doc))
    return meta


def offline_load(paths, model):
    """Load the complete metadata without a device.

    :param paths: The firmware paths for :func:`firmware_load`.
    :param model: The device model for the host-side metadata.
    :return: The dict mapping topic to metadata dict.  Device topics are
        device-relative, and global driver topics, such as m/, follow.
    """
    device, driver = host_metadata(model)
    meta = firmware_load(paths)
    meta.update(device)
    meta.update(driver)
    return meta


def meta_diff(actual, expected):
    """Compare two metadata dicts.

    :param actual: The metadata dict under test.
    :param expected: The reference metadata dict.
    :return: The list of difference description strings, empty when equal.
    """
    diffs = []
    for topic in sorted(set(actual) | set(expected)):
        if topic not in expected:
            diffs.append(f'+ {topic}')
        elif topic not in actual:
            diffs.append(f'- {topic}')
        else:
            a = actual[topic] or {}
            e = expected[topic] or {}
            for key in sorted(set(a) | set(e)):
                if a.get(key) != e.get(key):
                    diffs.append(f'~ {topic} {key}: {a.get(key)!r} != {e.get(key)!r}')
    return diffs


def to_json(meta):
    return json.dumps(meta, indent=2) + '\n'


def to_yaml(meta):
    import yaml
    return yaml.safe_dump(meta, sort_keys=True, allow_unicode=True)


def _columns(meta):
    keys = set()
    for value in meta.values():
        if value is not None:  # devices publish null metadata for command topics
            keys.update(value.keys())
    columns = [k for k in _COLUMN_ORDER if k in keys]
    columns += sorted(keys - set(_COLUMN_ORDER))
    return columns


def _cell(column, value):
    if value is None:
        return ''
    if column == 'options':
        parts = []
        for option in value:
            text = str(option[0])
            if len(option) > 1:
                text += f': {option[1]}'
            if len(option) > 2:
                text += ' (' + ', '.join(str(a) for a in option[2:]) + ')'
            parts.append(html.escape(text))
        return '<br>'.join(parts)
    if column == 'flags':
        return html.escape(', '.join(str(f) for f in value))
    return html.escape(str(value))


_HTML_STYLE = """\
  body { font-family: sans-serif; margin: 1em; }
  #cols { margin-bottom: 1em; }
  #cols label { margin-right: 1em; white-space: nowrap; }
  table { border-collapse: collapse; }
  th, td { border: 1px solid #999; padding: 0.25em 0.5em;
           text-align: left; vertical-align: top; }
  th { position: sticky; top: 0; background: #ddd; }
  tr:nth-child(even) { background: #eee; }
  td.topic { font-family: monospace; white-space: nowrap; }
  .hide { display: none; }
"""

_HTML_SCRIPT = """\
  document.querySelectorAll('#cols input').forEach((cb) => {
    cb.addEventListener('change', () => {
      document.querySelectorAll('.col-' + cb.dataset.col).forEach((el) => {
        el.classList.toggle('hide', !cb.checked);
      });
    });
  });
"""


def to_html(meta, title):
    columns = _columns(meta)
    title = html.escape(title)
    parts = [
        '<!DOCTYPE html>',
        '<html lang="en">',
        '<head>',
        '<meta charset="utf-8">',
        f'<title>{title}</title>',
        f'<style>\n{_HTML_STYLE}</style>',
        '</head>',
        '<body>',
        f'<h1>{title}</h1>',
        '<div id="cols">Columns:',
    ]
    for idx, column in enumerate(columns):
        checked = '' if column in _COLUMNS_HIDDEN else ' checked'
        parts.append(f'<label><input type="checkbox" data-col="{idx}"{checked}>'
                     + f'{html.escape(column)}</label>')
    parts += ['</div>', '<table>', '<thead>', '<tr>', '<th>topic</th>']
    for idx, column in enumerate(columns):
        hide = ' hide' if column in _COLUMNS_HIDDEN else ''
        parts.append(f'<th class="col-{idx}{hide}">{html.escape(column)}</th>')
    parts += ['</tr>', '</thead>', '<tbody>']
    for topic in sorted(meta.keys()):
        entry = meta[topic] if meta[topic] is not None else {}
        parts += ['<tr>', f'<td class="topic">{html.escape(topic)}</td>']
        for idx, column in enumerate(columns):
            hide = ' hide' if column in _COLUMNS_HIDDEN else ''
            parts.append(f'<td class="col-{idx}{hide}">{_cell(column, entry.get(column))}</td>')
        parts.append('</tr>')
    parts += ['</tbody>', '</table>',
              f'<script>\n{_HTML_SCRIPT}</script>',
              '</body>', '</html>', '']
    return '\n'.join(parts)


def on_cmd(args):
    fmt = format_resolve(args.format, args.out)
    if fmt == 'yaml':
        try:
            import yaml
        except ImportError:
            print('YAML output requires pyyaml: pip install pyyaml')
            return 1
    if args.firmware:
        try:
            meta = offline_load(args.firmware, args.model)
        except (OSError, ValueError, KeyError) as ex:
            print(ex)
            return 1
        title = f'{args.model} metadata, pyjoulescope_driver {__version__}'
    else:
        with Driver() as d:
            d.log_level = args.jsdrv_log_level
            try:
                device_path = device_select(d.device_paths(), args.device)
            except ValueError as ex:
                print(ex)
                return 1
            d.open(device_path, mode=args.open)
            try:
                meta = metadata_load(d, device_path)
            finally:
                d.close(device_path)
        title = f'{device_path} metadata'
    if args.diff:
        with open(args.diff, 'r', encoding='utf-8') as f:
            expected = json.load(f)
        # live devices do not publish the global driver topics
        _, driver = host_metadata(args.model)
        meta = {k: v for k, v in meta.items() if k not in driver}
        expected = {k: v for k, v in expected.items() if k not in driver}
        diffs = meta_diff(meta, expected)
        for line in diffs:
            print(line)
        return 1 if diffs else 0
    if fmt == 'json':
        out = to_json(meta)
    elif fmt == 'yaml':
        out = to_yaml(meta)
    else:
        out = to_html(meta, args.title or title)
    if args.out is None:
        print(out, end='')
    else:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(out)
    return 0
