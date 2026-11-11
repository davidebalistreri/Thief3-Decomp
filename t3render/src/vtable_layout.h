#pragma once
#include <stddef.h>

// C and C++ see COM interfaces differently. The C unit derives these sizes
// and offsets from the SDK's actual C vtable declarations.
typedef struct T3VtableLayout {
    size_t factory_size, factory_query, factory_release, factory_create_device;
    size_t device_size, device_query, device_release, device_reset, device_present, device_clear;
    size_t device_draw, device_draw_indexed, device_draw_up, device_draw_indexed_up;
} T3VtableLayout;

#ifdef __cplusplus
extern "C" {
#endif
T3VtableLayout T3GetVtableLayout(void);
#ifdef __cplusplus
}
#endif
