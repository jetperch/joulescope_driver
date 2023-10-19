const addon = require('../build/Release/joulescope_driver-native');

module.exports = {
    'initialize': addon.initialize,
    'finalize': addon.finalize
}

