"""Build small synthetic D3D8 examples from documented token encodings."""


def packed_declaration():
    # D3DVSD_STREAM(0), then D3DVSD_REG(register, type) for a generic
    # position/normal/UV/tangent/color layout, then D3DVSD_END().
    stream = 1 << 29
    stream_data = 2 << 29
    float2, float3, color = 1, 2, 4
    registers = [stream_data | (kind << 16) | number for number, kind in
                 enumerate((float3, color, float2, color, color))]
    return [stream, *registers, (1 << 32) - 1]


def position_shader():
    # Synthetic vs_1_0 program: dp4 oPos.{x,y,z,w}, v0, c{1,2,3,4}.
    # The bit fields are the standard D3D8 parameter marker, register type,
    # write mask, register number and identity swizzle.
    version = (0xfffe << 16) | (1 << 8)
    marker = 1 << 31
    identity_swizzle = (0 | 1 << 2 | 2 << 4 | 3 << 6) << 16
    position = marker | (1 << 28) | identity_swizzle
    instructions = []
    for axis in range(4):
        output = marker | (4 << 28) | (1 << (16 + axis))
        matrix_row = marker | (2 << 28) | identity_swizzle | (axis + 1)
        instructions.extend([9, output, position, matrix_row])
    return [version, *instructions, (1 << 16) - 1]
