#pragma once
#include <stddef.h>

// C and C++ see COM interfaces differently. The C unit derives these sizes
// and offsets from the SDK's actual C vtable declarations.
typedef struct T3VtableLayout {
    size_t factory_size, factory_query, factory_release, factory_create_device;
    size_t device_size, device_query, device_release, device_reset, device_present, device_clear;
    size_t device_draw, device_draw_indexed, device_draw_up, device_draw_indexed_up;
    size_t device_create_vertex_buffer, device_create_index_buffer;
    size_t device_set_stream, device_get_stream, device_set_indices, device_get_indices, device_process_vertices;
    size_t device_begin_state_block, device_end_state_block, device_apply_state_block;
    size_t device_capture_state_block, device_delete_state_block, device_create_state_block;
    size_t device_create_vertex_shader, device_set_vertex_shader, device_get_vertex_shader, device_delete_vertex_shader;
    size_t device_set_vertex_shader_constant, device_get_vertex_shader_constant;
} T3VtableLayout;

#ifdef __cplusplus
extern "C" {
#endif
T3VtableLayout T3GetVtableLayout(void);
#ifdef __cplusplus
}
#endif
