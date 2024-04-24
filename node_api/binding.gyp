{
  'targets': [
    {
      'target_name': 'joulescope_driver_native',
      'sources': [
        'src/addon.cc',
        'src/joulescope_driver.cc',
        '../src/buffer.c',
        '../src/buffer_signal.c',
        '../src/cstr.c',
        '../src/devices.c',
        '../src/downsample.c',
        '../src/error_code.c',
        '../src/js110_cal.c',
        '../src/js110_sample_processor.c',
        '../src/js110_stats.c',
        '../src/js110_usb.c',
        '../src/js220_i128.c',
        '../src/js220_params.c',
        '../src/js220_usb.c',
        '../src/js220_stats.c',
        '../src/jsdrv.c',
        '../src/json.c',
        '../src/log.c',
        '../src/pubsub.c',
        '../src/meta.c',
        '../src/sample_buffer_f32.c',
        '../src/statistics.c',
        '../src/time.c',
        '../src/time_map_filter.c',
        '../src/topic.c',
        '../src/union.c',
        '../src/version.c',
        '../third-party/tinyprintf/tinyprintf.c'
      ],
      'conditions': [
        ["OS=='win'", {
          'sources': [
            '../src/backend/winusb/backend.c',
            '../src/backend/winusb/device_change_notifier.c',
            '../src/backend/winusb/msg_queue.c',
            '../src/backend/windows.c'
          ],
          'libraries': ['Setupapi', 'Winusb', 'user32', 'winmm']
        }],
        ["OS!='win'", {
          'sources': [
            '../src/backend/posix.c',
            '../src/backend/libusb/backend.c',
            '../src/backend/libusb/msg_queue.c',
            '../third-party/libusb/libusb/core.c',
            '../third-party/libusb/libusb/descriptor.c',
            '../third-party/libusb/libusb/hotplug.c',
            '../third-party/libusb/libusb/io.c',
            '../third-party/libusb/libusb/strerror.c',
            '../third-party/libusb/libusb/sync.c',
            '../third-party/libusb/libusb/os/events_posix.c',
            '../third-party/libusb/libusb/os/threads_posix.c'
          ],
          'libraries': [],
          "conditions": [
            ["OS=='mac'", {
              'sources': [
                '../third-party/libusb/libusb/os/darwin_usb.c'
              ],
              'include_dirs': [
                '../third-party/libusb/libusb',
                '../third-party/libusb/include/macos'
              ]
            }],
            ["OS=='linux'", {
              "sources": [
                '../third-party/libusb/libusb/os/linux_udev.c',
                '../third-party/libusb/libusb/os/linux_usbfs.c',
              ],
              'include_dirs': [
                '../third-party/libusb/libusb',
                '../third-party/libusb/include/linux'
              ],
              'libraries': []
            }]
          ]
        }]
      ],
      'include_dirs': [
        "<!@(node -p \"require('node-addon-api').include\")",
        "../include",
        "../include_private",
        "../third-party/tinyprintf"
      ],
      'dependencies': ["<!(node -p \"require('node-addon-api').gyp\")"],
      'cflags!': [ '-fno-exceptions' ],
      'cflags_cc!': [ '-fno-exceptions' ],
      'xcode_settings': {
        'GCC_ENABLE_CPP_EXCEPTIONS': 'YES',
        'CLANG_CXX_LIBRARY': 'libc++',
        'MACOSX_DEPLOYMENT_TARGET': '10.7'
      },
      'msvs_settings': {
        'VCCLCompilerTool': { 'ExceptionHandling': 1 },
      }
    }
  ]
}