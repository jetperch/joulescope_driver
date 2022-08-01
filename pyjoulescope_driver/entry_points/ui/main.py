# Copyright 2020-2022 Jetperch LLC
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


from pyjoulescope_driver import Driver, __version__, __url__
from .resync import Resync
from .device_widget import DeviceWidget
from .log_widget import LogWidget
from .waveform import WaveformWidget
from .vertical_scroll_area import VerticalScrollArea
from PySide6 import QtCore, QtGui, QtWidgets
import logging
import sys


log = logging.getLogger(__name__)
STATUS_BAR_TIMEOUT_DEFAULT = 2500
PORT_COUNT = 32


ABOUT = """\
<html>
<head>
</head>
<body>
Joulescope Driver Development UI<br/> 
Version {version}<br/>
<a href="{url}">{url}</a>

<pre>
Copyright 2018-2022 Jetperch LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
</pre>
</body>
</html>
"""


def _menu_setup(d, parent=None):
    k = {}
    for name, value in d.items():
        name_safe = name.replace('&', '')
        if isinstance(value, dict):
            wroot = QtWidgets.QMenu(parent)
            wroot.setTitle(name)
            parent.addAction(wroot.menuAction())
            w = _menu_setup(value, wroot)
            w['__root__'] = wroot
        else:
            w = QtGui.QAction(parent)
            w.setText(name)
            if callable(value):
                w.triggered.connect(value)
            parent.addAction(w)
        k[name_safe] = w
    return k


class MainWindow(QtWidgets.QMainWindow):

    def __init__(self, app, args):
        self._driver = None
        self._device_path = None
        super(MainWindow, self).__init__()
        self._resync = Resync(self)
        self._resync_driver_cmd = self._resync.wrap(self._on_driver_cmd)
        self._resync_driver_device = self._resync.wrap(self._on_driver_device)
        self.setObjectName('MainWindow')
        self.setWindowTitle('Joulescope Driver UI')
        self.resize(640, 480)

        self._central_widget = QtWidgets.QSplitter(QtCore.Qt.Vertical, self)
        self._central_widget.setObjectName('central')
        self._central_widget.setSizePolicy(QtWidgets.QSizePolicy.Expanding,
                                           QtWidgets.QSizePolicy.Expanding)
        self.setCentralWidget(self._central_widget)

        self._hwidget = QtWidgets.QWidget(self._central_widget)
        self._central_widget.addWidget(self._hwidget)
        self._central_widget.setStretchFactor(0, 3)

        self._hlayout = QtWidgets.QHBoxLayout(self._hwidget)
        self._hlayout.setSpacing(6)
        self._hlayout.setContentsMargins(11, 11, 11, 11)
        self._hlayout.setObjectName('central_layout')

        self._device_scroll = VerticalScrollArea(self._hwidget)
        self._hlayout.addWidget(self._device_scroll)

        self._device_widget = DeviceWidget(self._device_scroll)
        self._device_scroll.setWidget(self._device_widget)

        self._waveforms = []
        for _ in range(2):
            waveform = WaveformWidget(self._hwidget)
            size_policy = QtWidgets.QSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
            size_policy.setHorizontalStretch(1)
            waveform.setSizePolicy(size_policy)
            self._hlayout.addWidget(waveform)
            self._waveforms.append(waveform)

        self._log_widget = LogWidget(self._central_widget)
        self._central_widget.addWidget(self._log_widget)
        self._central_widget.setStretchFactor(1, 1)

        # Status bar
        self._status_bar = QtWidgets.QStatusBar(self)
        self.setStatusBar(self._status_bar)
        self._status_indicator = QtWidgets.QLabel(self._status_bar)
        self._status_indicator.setObjectName('status_indicator')
        self._status_bar.addPermanentWidget(self._status_indicator)

        # status update timer
        #self._status_update_timer = QtCore.QTimer(self)
        #self._status_update_timer.setInterval(1000)  # milliseconds
        #self._status_update_timer.timeout.connect(self._on_status_update_timer)
        #self._status_update_timer.start()

        self._menu_bar = QtWidgets.QMenuBar(self)
        self._menu_items = _menu_setup(
            {
                '&File': {
                    'E&xit': self._on_file_exit,
                },
                '&Help': {
                    '&Credits': self._on_help_credits,
                    '&About': self._on_help_about,
                }
            },
            self._menu_bar)
        self.setMenuBar(self._menu_bar)
        self.show()
        self._driver = Driver()
        self._driver.subscribe("@", ['pub_retain', 'metadata_rsp_retain'], self._resync_driver_cmd)
        self._driver.log_level = args.jsdrv_log_level

        #device_paths = self._driver.device_paths()
        #if len(device_paths) == 1:
        #    self._on_device_open(device_paths[0])

    def _on_device_open(self, device_path):
        self._driver.open(device_path)
        self._device_path = device_path
        self._device_widget.open(self._driver, self._device_path)
        self._driver.subscribe(self._device_path, 'pub_retain', self._resync_driver_device)

    def _on_device_close(self):
        if self._device_path is not None:
            self._device_widget.close()
            device_path, self._device_path = self._device_path, None
            try:
                self._driver.close(device_path)
            except Exception:
                # presume closed despite error & delegate error recovery to driver
                log.exception(f'self._driver.close({device_path})')

    def _on_driver_cmd(self, *args, **kwargs):
        log.debug(f'_on_driver_cmd {args} {kwargs}')
        topic, value = args
        if topic == '@/!add':
            #if value.startswith('u/js220/') and self._device_path is None:
            self._on_device_open(value)
        elif topic == '@/!remove':
            if value == self._device_path:
                self._on_device_close()

    def _on_driver_device(self, *args, **kwargs):
        topic, value = args
        if topic.endswith('!data'):
            for waveform in self._waveforms:
                waveform.on_pub(topic, value)
        else:
            log.debug(f'_on_driver_device {args} {kwargs}')
            self._device_widget.on_pub(*args, **kwargs)

    def closeEvent(self, event):
        log.info('closeEvent()')
        self._on_device_close()
        log.info('closeEvent() - finalize driver')
        self._driver.finalize()
        log.info('closeEvent() - close window')
        return super(MainWindow, self).closeEvent(event)

    def _on_file_exit(self):
        log.info('_on_file_exit')
        self.close()

    def _on_help_credits(self):
        log.info('_on_help_credits')

    def _on_help_about(self):
        log.info('_on_help_about')
        txt = ABOUT.format(version=__version__,
                           url=__url__)
        QtWidgets.QMessageBox.about(self, 'Joulescope Driver UI', txt)


def run(args):
    app = QtWidgets.QApplication(sys.argv)
    ui = MainWindow(app, args)
    rc = app.exec()
    del ui
    del app
    return rc
