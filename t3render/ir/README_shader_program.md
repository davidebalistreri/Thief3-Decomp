# D3D8 vertex program framing

`ScanVertexShader11` is a bounded reader for application-provided `vs_1_1`
bytecode. It frames known instructions, skips the full COMMENT and DEF
payloads even if they contain an END-looking word, and fails closed on
unsupported versions, unknown instructions, unreadable bytes and budget or
address limits. A null function pointer is reported separately. This is not a
shader validator, translator or proof of the program used by a driver.

The declaration registry can optionally retain one program snapshot alongside
its declaration and return it with a stable candidate. Both share the device's
word budget and lifecycle. Eligible pre-reset programs are kept separately as
hypotheses, never as candidates for the device after Reset.
Malformed or oversized programs expose no words. Registry tests exercise quota,
deletion and reset; the caller still has to check the completed draw ticket.

The optional proxy observer samples at `CreateVertexShader` and writes bounded
program candidates into the diagnostic trace. The offline IR retains them only
with a matching declaration generation and a successful, stable draw. The
caller must supply a fault-safe reader; a snapshot may race application writes.
Other shader versions, pixel shaders, constants and unobserved device aliases
remain unhandled. No real TDS shader has yet been captured or replayed by this
observer.

From `t3sdk`, run the host framing checks and the i686 cross-compile check:

```sh
python3 -B -m unittest t3render.tests.test_shader_program -v
```
