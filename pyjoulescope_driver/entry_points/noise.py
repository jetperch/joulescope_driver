# Copyright 2022 Jetperch LLC
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

from pyjoulescope_driver import Driver
import numpy as np
import queue
import signal
import time
import threading


_quit = False


def parser_config(p):
    """Joulescope noise analysis."""
    p.add_argument('--plot', '-p',
                   action='store_true',
                   help='Plot the waveform.')
    return on_cmd


def _on_signal(*args, **kwargs):
    global _quit
    _quit = True


def _plot_one(plt, v):
    f = plt.figure(figsize=[8.0, 6.0], layout='tight')

    ax = f.add_subplot(3, 2, 1)
    ax.set_title('Time Domain')
    ax.set_xlabel('Time (samples)')
    ax.set_ylabel('Value (LSBs)')
    ax.grid(True)
    ax.plot(v[:400])

    ax = f.add_subplot(3, 2, 2)
    ax.set_title('Time Domain')
    ax.set_xlabel('Time (samples)')
    ax.set_ylabel('Value (LSBs)')
    ax.grid(True)
    ax.plot(v)

    ax = f.add_subplot(3, 1, 2)
    ax.set_title('Histogram')
    ax.set_xlabel('Value (LSBs)')
    ax.set_ylabel('Counts')
    ax.grid(True)
    v_min = int(np.min(v))
    v_max = int(np.max(v))
    ax.hist(v, bins=range(v_min, v_max+1))

    fs = 2e6
    x = np.arange(len(v) // 2 + 1) * (fs / len(v))
    v = v.astype(np.float64) * 2**-15
    v -= np.mean(v)

    ax = f.add_subplot(3, 1, 3)
    ax.set_title('FFT')
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Value (dB)')
    ax.grid(True)
    z = np.fft.rfft(v)
    y = np.real((z * np.conj(z)))
    y = 20 * np.log10(y)  # dB
    ax.plot(x[1:], y[1:])
    return f


class App:
    def __init__(self, args):
        self._args = args
        self._driver = None
        self._queue = queue.Queue()
        self._device_queue = queue.Queue()
        self._device_prefix = None
        self._thread = None

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        pass

    def _on_dev(self, topic, value):
        print(f'on_dev {topic} {value}')
        self._device_queue.put((topic, value), block=False)

    def _dev_clear(self, duration=None):
        try:
            if duration is None:
                while True:
                    self._device_queue.get(block=False)
            else:
                t_start = time.time()
                t_end = t_start + float(duration)
                while True:
                    t_remain = t_end - time.time()
                    if t_remain < 0:
                        break
                    self._device_queue.get(block=True, timeout=t_remain)
        except queue.Empty:
            pass

    def _dev_await(self, topic, timeout=None):
        global _quit
        timeout = 1.0 if timeout is None else float(timeout)
        t_start = time.time()
        while not _quit:
            try:
                t, v = self._device_queue.get(block=True, timeout=0.1)
                if topic == t:
                    return v
            except queue.Empty:
                pass
            if time.time() - t_start > timeout:
                raise TimeoutError('dev_await')
        raise KeyboardInterrupt('quit!')

    def run_device(self):
        data = []
        p = self._device_prefix
        sub_fn = self._on_dev
        self._driver.subscribe(p, 'pub', sub_fn)
        self._driver.publish(p + "/@/!open", 0)
        self._dev_clear(0.75)
        self._driver.publish(p + "/c/led/blue", 0xff)
        self._driver.publish(p + "/c/led/en", 1)
        self._driver.publish(p + "/s/led/blue", 0xff)
        self._driver.publish(p + "/s/led/en", 1)
        self._driver.publish(p + "/s/i/range/select", 0x81)  # Z on, 10 A on
        self._driver.publish(p + "/s/i/range/mode", 7)       # manual
        self._dev_clear(0.1)

        if True:
            self._driver.publish(p + "/s/adc/0/ctrl", 1)
            data.append(self._dev_await(p + "/s/adc/0/!data"))
            self._driver.publish(p + "/s/adc/0/ctrl", 0)
            self._dev_clear(0.1)

        self._driver.publish(p + "/s/adc/1/ctrl", 1)
        data.append(self._dev_await(p + "/s/adc/1/!data"))
        self._driver.publish(p + "/s/adc/1/ctrl", 0)
        self._dev_clear(0.1)

        if True:
            self._driver.publish(p + "/s/adc/2/ctrl", 1)
            for select in range(6):
                self._driver.publish(p + "/s/i/range/select", 0x80 + (1 << select))
                self._dev_clear(0.1)
                data.append(self._dev_await(p + "/s/adc/2/!data"))
            self._driver.publish(p + "/s/adc/2/ctrl", 0)
            self._dev_clear(0.1)
            self._driver.publish(p + "/s/i/range/select", 0x82)

        if True:
            self._driver.publish(p + "/s/adc/3/ctrl", 1)
            data.append(self._dev_await(p + "/s/adc/3/!data"))
            self._driver.publish(p + "/s/adc/3/ctrl", 0)
            self._dev_clear(0.1)

        self._driver.unsubscribe(p, sub_fn)
        self._dev_clear()
        for idx, v in enumerate(data):
            v_mean = np.mean(v['data'], dtype=np.float64)
            v_std = np.std(v['data'], dtype=np.float64)
            print(f'{idx}: mean={v_mean:.2f}, std={v_std:.2f}')

        self._queue.put(('__done__', data), block=False)

    def _on_cmd(self, topic, value):
        self._queue.put((topic, value), block=False)

    def _handle_cmd(self, topic, value):
        global _quit
        print(f'on_cmd {topic} {value}')
        if topic == '@/!add':
            if self._device_prefix is not None:
                print('Found second device, ignore')
                return
            self._device_prefix = value
            self._thread = threading.Thread(name='device', target=self.run_device)
            self._thread.start()
        if topic == '__done__':
            self._thread.join()
            self._thread = None
            _quit = True
            if self._args.plot:
                import matplotlib.pyplot as plt
                for v in value:
                    _plot_one(plt, v['data'])
                plt.show()

    def run(self):
        global _quit
        sub_fn = self._on_cmd
        with Driver() as d:
            self._driver = d
            d.subscribe('@', 'pub', sub_fn)
            while not _quit:
                try:
                    topic, value = self._queue.get(block=True, timeout=0.1)
                except queue.Empty:
                    continue
                self._handle_cmd(topic, value)
            d.unsubscribe('@', sub_fn)
        self._driver = None


def on_cmd(args):
    signal.signal(signal.SIGINT, _on_signal)
    with App(args) as a:
        a.run()
