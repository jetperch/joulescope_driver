# Copyright 2021 Jetperch LLC
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


from PySide6 import QtCore, QtGui, QtWidgets
import logging


PORTS_COUNT = 32  # todo
DTYPES = ['str', 'json', 'bin',
          'f32', 'f64',
          'bool',
          'u8', 'u16', 'u32', 'u64',
          'i8', 'i16', 'i32', 'i64']
FLAGA = ['ro', 'hide', 'dev']
log = logging.getLogger(__name__)
SPACING = 5
MARGINS = (15, 0, 5, 5)

V_MIN = {
    'u8': 0,
    'u16': 0,
    'u32': 0,
    'u64': 0,
    'i8': -2**7,
    'i16': -2**15,
    'i32': -2**31,
    'i64': -2**64,
}


V_MAX = {
    'u8': 2**8 - 1,
    'u16': 2**16 - 1,
    'u32': 2**32 - 1,
    'u64': 2**64 - 1,
    'i8': 2**7 - 1,
    'i16': 2**15 - 1,
    'i32': 2**31 - 1,
    'i64': 2**64 - 1,
}


class DeviceWidget(QtWidgets.QWidget):

    def __init__(self, parent):
        super(DeviceWidget, self).__init__(parent)
        self._layout = QtWidgets.QVBoxLayout(self)
        self._layout.setSpacing(SPACING)
        self._layout.setContentsMargins(5, 0, 5, 5)
        self._layout.setObjectName('device_layout')

        self._pubsub_widget = PubSubWidget(self)
        self._layout.addWidget(self._pubsub_widget)

        self.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
                           QtWidgets.QSizePolicy.Preferred)
        # print(f'device sizeHint={self.sizeHint()}, minimumSize={self.minimumSize()}')

    def minimumSize(self):
        return self.sizeHint()

    def open(self, driver, device_path):
        self._pubsub_widget.open(driver, device_path)

    def close(self):
        self._pubsub_widget.close()

    def on_pub(self, topic, value):
        self._pubsub_widget.on_pub(topic, value)


class Value(QtCore.QObject):

    def __init__(self, parent, topic, meta):
        QtCore.QObject.__init__(self, parent)
        self._parent = parent
        self._topic = topic
        self._meta = meta
        self._value = None
        self._options = None
        self.label = None
        self.editor = None
        # See include/jsdrv.h for the latest format information
        if meta is None:
            log.warning('topic %s: missing metadata', topic)
            return
        dtype = meta.get('dtype')
        if dtype not in DTYPES:
            log.warning('topic %s: unsupported dtype %s', topic, dtype)
            return
        name = topic
        brief = meta.get('brief')
        detail = meta.get('detail')
        default = meta.get('default')
        options = meta.get('options')
        flags = meta.get('flags')
        flags = [] if flags is None else flags
        # print(f'META: topic={topic}, dtype={dtype}, brief={brief}, detail={detail}, default={default}, options={options}, flags={flags}')

        self.label = QtWidgets.QLabel(name, parent)
        tooltip = f'<html><body><p>{brief}</p>'
        if detail is not None:
            tooltip += f'<p>{detail}</p>'
        tooltip += f'</body></html>'
        self.label.setToolTip(tooltip)

        if 'ro' in flags:
            self.editor = QtWidgets.QLabel(parent)
        elif options is not None and len(options):
            self._options = []
            self.editor = QtWidgets.QComboBox(parent)
            for idx, opts in enumerate(options):
                v = opts[0]
                if len(opts) > 1:
                    e = str(opts[1])
                else:
                    e = str(v)
                self._options.append((v, e))
                self.editor.addItem(e)
            self.editor.currentIndexChanged.connect(self._on_combobox)
        elif dtype == 'str':
            self.editor = QtWidgets.QTextEdit(parent)
        elif dtype == 'json':
            self.editor = QtWidgets.QLabel("json: unsupported", parent)
        elif dtype == 'bin':
            self.editor = QtWidgets.QLabel("bin: unsupported", parent)
        #elif dtype in ['f32', 'f64']:
        #    pass
        elif dtype in ['u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64']:
            v_min, v_max, v_step = V_MIN[dtype], V_MAX[dtype], 1
            drange = self._meta.get('range')
            if drange is None:
                pass
            elif len(drange) == 2:
                v_min, v_max = drange
            elif len(drange) == 3:
                v_min, v_max, v_step = drange
            else:
                raise RuntimeError('topic %s: invalid range %s', self._topic, drange)
            self.editor = QtWidgets.QSpinBox(parent)
            if 'ro' not in flags and v_max < 2**31:
                self.editor.setRange(v_min, v_max)
                self.editor.setSingleStep(v_step)
            self.editor.valueChanged.connect(self._on_spinbox)
        elif dtype == 'bool':
            self.editor = QtWidgets.QCheckBox(parent)
            self.editor.clicked.connect(self._on_clicked)
        else:
            self.editor = QtWidgets.QLabel("value", parent)
        self.editor.setToolTip(tooltip)
        if 'hide' in flags:
            self.setVisible(False)
        # if 'dev' in flags:
        #     self.setVisible(False)
        if default is not None:
            self.value = default

    def setVisible(self, visible):
        if self.label is not None:
            self.label.setEnabled(visible)
            self.label.setVisible(visible)
        if self.editor is not None:
            self.editor.setEnabled(visible)
            self.editor.setVisible(visible)

    @property
    def value(self):
        return self._value

    def _find_option_idx(self, x):
        for idx, k in enumerate(self._options):
            if x in k:
                return idx
        raise RuntimeError(f'topic {self._topic}: option {x} not found')

    @value.setter
    def value(self, x):
        dtype = self._meta.get('dtype')
        b = self.editor.blockSignals(True)
        try:
            if 'ro' in self._meta.get('flags', []):
                if dtype == 'u32' and self._meta.get('format') == 'version':
                    x = int(x)
                    major = (x >> 24) & 0xff
                    minor = (x >> 16) & 0xff
                    patch = x & 0xffff
                    x = f'{major}.{minor}.{patch}'
                self.editor.setText(str(x))
            elif self._options is not None:
                idx = self._find_option_idx(x)
                self.editor.setCurrentIndex(idx)
            elif dtype == 'str':
                self.editor.setText(str(x))
            elif dtype in ['json', 'bin']:
                pass
            elif dtype in ['f32', 'f64']:
                pass
            elif dtype in ['u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64']:
                self.editor.setValue(int(x))
            elif dtype == 'bool':
                self.editor.setChecked(bool(x))
            else:
                pass
        finally:
            self._value = x
            self.editor.blockSignals(b)

    def _publish(self, value):
        self._parent.publish(self._topic, value)

    def _on_combobox(self, idx):
        opts = self._meta.get('options')[idx]
        self._publish(opts[0])

    def _on_clicked(self, value):
        self._publish(bool(value))

    def _on_spinbox(self, value):
        self._publish(value)


class PubSubWidget(QtWidgets.QWidget):

    def __init__(self, parent):
        self._driver = None
        self._device_path = None
        self._fn = None
        self._meta: dict[str, str] = {}
        self._values: dict[str, Value] = {}
        super(PubSubWidget, self).__init__(parent)
        self.setObjectName('pubsub_widget')
        self.setGeometry(QtCore.QRect(0, 0, 294, 401))

        self._rows = 0
        self._layout = QtWidgets.QGridLayout(self)
        self._layout.setSpacing(SPACING)
        self._layout.setContentsMargins(*MARGINS)
        self._layout.setObjectName('pubsub_widget_layout')

    def _meta_populate(self):
        for topic, meta in self._meta.items():
            value = Value(self, topic, meta)
            if value.label is not None:
                self._values[topic] = value
                self._layout.addWidget(value.label, self._rows, 0)
                self._layout.addWidget(value.editor, self._rows, 1)
                self._rows += 1
                self._layout.update()

    def open(self, driver, device_path):
        if device_path is None:
            self.close()
            return
        self._driver = driver
        self._device_path = device_path
        fn = self._on_meta
        driver.subscribe(self._device_path, 'metadata_rsp_retain', fn)
        driver.unsubscribe(self._device_path, fn)
        self._meta_populate()

    def close(self):
        if self._device_path:
            self.clear()
            self._device_path = None
            self._fn = None
            self._driver = None

    def clear(self):
        while not self._layout.isEmpty():
            item = self._layout.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.setParent(None)
        self._values.clear()
        self._meta.clear()

    def publish(self, topic, value):
        if self._device_path is None:
            return
        topic = f'{self._device_path}/{topic}'
        self._driver.publish(topic, value)

    def on_pub(self, topic, value):
        if not bool(self._device_path):
            return
        topic = topic[len(self._device_path) + 1:]
        v = self._values.get(topic)
        if v is not None:
            v.value = value

    def _on_meta(self, topic, meta):
        topic = topic[len(self._device_path) + 1:]
        if topic[-1] == '$':
            topic = topic[:-1]
        self._meta[topic] = meta
