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

struct jsdrvp_ul_device_s;
struct jsdrv_context_s;
struct jsdrvp_ll_device_s;

struct device_type_s {
    const char * model;
    GUID const guid;      // The GUID for the Microsoft WinUSB descriptor.
    uint16_t vendor_id;   // The 2-byte USB vendor identifier integer.
    uint16_t product_id;  // The 2-byte USB product identifier integer.

    /// Device factory. NULL for devices with no upper-level driver (e.g. bootloaders).
    int32_t (*device_factory)(struct jsdrvp_ul_device_s ** device,
                              struct jsdrv_context_s * context,
                              struct jsdrvp_ll_device_s * ll);
};

extern const struct device_type_s device_types[];

/**
 * @brief Look up a device type by model string.
 *
 * @param model The model string (e.g., "js320", "mb").
 * @return The device type entry, or NULL if not found.
 */
const struct device_type_s * device_type_lookup(const char * model);

/** @} */

JSDRV_CPP_GUARD_END

#endif /* JSDRV_BACKEND_DEVICES_H__ */
