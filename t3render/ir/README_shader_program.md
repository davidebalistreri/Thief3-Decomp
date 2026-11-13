# D3D8 vertex program framing

`ScanVertexShader11` is a bounded reader for application-provided `vs_1_1`
bytecode. It frames known instructions, skips the full COMMENT and DEF
payloads even if they contain an END-looking word, and fails closed on
unsupported versions, unknown instructions, unreadable bytes and budget or
address limits. A null function pointer is reported separately. This is not a
shader validator, translator or proof of the program used by a driver.

It is **not integrated into the proxy or trace**. The caller must supply a
fault-safe reader; its snapshot may race application writes. Other shader
versions, pixel shaders, constants, COM lifetime and device aliases remain
unhandled. No real TDS shader has been captured or replayed by this helper.

From `t3sdk`, run the host framing checks and the i686 cross-compile check:

```sh
python3 -B -m unittest t3render.tests.test_shader_program -v
```
