# Copyright 2024 Jetperch LLC
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


import argparse
import matplotlib.pyplot as plt
import numpy as np
import os


def get_parser():
    p = argparse.ArgumentParser(
        description='Display binary samples data captured by jsdrv.')
    p.add_argument('filename',
                   help='The binary data filename to load.')
    p.add_argument('--sample_rate',
                   type=float,
                   default=1_000_000.0,
                   help='The sample rate in samples per second (Hz).')
    return p


def run():
    args = get_parser().parse_args()
    d = np.fromfile(args.filename, dtype='<f4')
    f = plt.figure()
    ax = f.add_subplot(1, 1, 1)
    ax.set_title(os.path.basename(args.filename))
    y = d
    x = np.arange(len(y)) * (1.0 / args.sample_rate)
    ax.grid(True)
    ax.plot(x, y)
    ax.set_xlabel('Time (seconds)')
    plt.show()


if __name__ == '__main__':
    run()
