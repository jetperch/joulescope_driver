/* Copyright 2022 Jetperch LLC.  All rights reserved. */

/**
 * @file
 *
 * @brief The supported device definitions.
 */

#ifndef JSDRV_BACKEND_DEVICES_H__
#define JSDRV_BACKEND_DEVICES_H__

#include "jsdrv/cmacro_inc.h"
#include <stdint.h>
#include <stdbool.h>

#if _WIN32
#include <windows.h>
#endif

JSDRV_CPP_GUARD_START

/**
 * @ingroup jsdrv
 * @defgroup jsdrvb_devices
 *
 * @brief The supported device definitions.
 *
 * @{
 */

#if _WIN32
#else
typedef struct _GUID {
    unsigned long  Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char  Data4[ 8 ];
} GUID;
#endif

enum jsdrv_device_type_e {
    JSDRV_DEVICE_TYPE_INVALID,
    JSDRV_DEVICE_TYPE_JS110_APP,
    JSDRV_DEVICE_TYPE_JS110_BL,
    JSDRV_DEVICE_TYPE_JS220_APP,
    JSDRV_DEVICE_TYPE_JS220_BL,
};

struct device_type_s {
    enum jsdrv_device_type_e device_type;
    const char * model;
    GUID const guid;      // The GUID for the Microsoft WinUSB descriptor.
    uint16_t vendor_id;   // The 2-byte USB vendor identifier integer.
    uint16_t product_id;  // The 2-byte USB product identifier integer.
};

extern const struct device_type_s device_types[];

/** @} */

JSDRV_CPP_GUARD_END

#endif /* JSDRV_BACKEND_DEVICES_H__ */
