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


import pyqtgraph as pg
from PySide6 import QtCore, QtGui, QtWidgets
import logging
import numpy as np


class WaveformWidget(QtWidgets.QWidget):

    def __init__(self, parent=None):
        super(WaveformWidget, self).__init__(parent)
        self._topics = {}
        self._topic = None
        self._log = logging.getLogger(__name__)

        self._layout = QtWidgets.QVBoxLayout(self)
        self._layout.setSpacing(0)
        self._layout.setContentsMargins(0, 0, 0, 0)

        self._source_widget = QtWidgets.QWidget(self)
        self._source_layout = QtWidgets.QHBoxLayout(self._source_widget)
        self._source_label = QtWidgets.QLabel('Source: ', parent=self._source_widget)
        self._source_layout.addWidget(self._source_label)
        self._source_combobox = QtWidgets.QComboBox(self._source_widget)
        self._source_layout.addWidget(self._source_combobox)
        self._layout.addWidget(self._source_widget)

        self.win = pg.GraphicsLayoutWidget(parent=self, show=True, title="Waveform")
        self.win.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        # self.win.sceneObj.sigMouseClicked.connect(self._on_mouse_clicked_event)
        # self.win.sceneObj.sigMouseMoved.connect(self._on_mouse_moved_event)
        self._layout.addWidget(self.win)

        self._bottom_widget = QtWidgets.QWidget(self)
        self._bottom_layout = QtWidgets.QGridLayout(self._bottom_widget)
        self._avg_widget = QtWidgets.QLabel("avg=0.000", parent=self._bottom_widget)
        self._std_widget = QtWidgets.QLabel("std=0.000", parent=self._bottom_widget)
        self._min_widget = QtWidgets.QLabel("min=0.000", parent=self._bottom_widget)
        self._max_widget = QtWidgets.QLabel("max=0.000", parent=self._bottom_widget)
        self._p2p_widget = QtWidgets.QLabel("p2p=0.000", parent=self._bottom_widget)
        self._bottom_layout.addWidget(self._avg_widget, 0, 0)
        self._bottom_layout.addWidget(self._std_widget, 0, 1)
        self._bottom_layout.addWidget(self._min_widget, 0, 2)
        self._bottom_layout.addWidget(self._max_widget, 0, 3)
        self._bottom_layout.addWidget(self._p2p_widget, 0, 4)
        self._layout.addWidget(self._bottom_widget)

        self._time_plot: pg.PlotItem = self.win.addPlot(row=0, col=0)
        self._time_plot.showGrid(True, True, 128)
        self._time_curve = self._time_plot.plot(pen='y')

        self._freq_plot: pg.PlotItem = self.win.addPlot(row=1, col=0)
        self._freq_plot.showGrid(True, True, 128)
        self._freq_curve = self._freq_plot.plot(pen='y')

        self._data = []
        self._index = 0

        self._timer = QtCore.QTimer()
        self._timer.timeout.connect(self._redraw)
        self._timer.start(50)

    def _redraw(self):
        if len(self._data):
            num_points = 4096
            data = np.concatenate(self._data)
            self._data.clear()
            d_display = data[-num_points:]
            self._time_curve.setData(d_display)
            d_avg, d_std, d_min, d_max = np.mean(data), np.std(data), np.min(data), np.max(data)
            self._avg_widget.setText(f'avg={d_avg:.3g}')
            self._std_widget.setText(f'std={d_std:.3g}')
            self._min_widget.setText(f'min={d_min:.3g}')
            self._max_widget.setText(f'max={d_max:.3g}')
            self._p2p_widget.setText(f'p2p={d_max - d_min:.3g}')

            try:
                x = np.arange(num_points // 2 + 1) * (2e6 / num_points)
                z = np.fft.rfft(d_display - np.mean(d_display))
                y = np.real((z * np.conj(z)))
                y[y < 1e-15] = 1e-15
                y = 20 * np.log10(y)
                self._freq_curve.setData(x[:1024], y[:1024])
            except Exception:
                self._log.warning('Could not compute FFT')

    def on_pub(self, topic, value):
        if not topic.endswith('/!data'):
            return
        v = self._topics.get(topic, 0)
        if v == 0:
            self._log.info('Added waveform source: %s', topic)
            self._source_combobox.addItem(topic)
        self._topics[topic] = v + 1
        if topic == self._source_combobox.currentText():
            # print(f'found it {value}')
            d_next = value['data']
            self._data.append(d_next)
