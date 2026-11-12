# Fixture-only D3D8 device facade

This is an isolated COM forwarding experiment. It is not integrated into the
proxy or TDS and must not be used in TDS. It does not wrap resources: a native
resource's `GetDevice`, `GetContainer` or `QueryInterface` can expose native
interfaces. Passing wrapped resources through those aliases is unsafe.
State blocks retain native bindings and are forwarded without identity tracking.

`CreateDeviceFacadeFactory(native)` adopts the caller's native factory +1 only
on success; on null or allocation failure it returns null and leaves ownership
with the caller. Use it once per native factory identity: it does not deduplicate
separate aliases. The factory calls native `CreateDevice` and wraps its returned
device; on wrapper allocation failure the successfully returned native device
is released and `E_OUTOFMEMORY` is returned.

`IUnknown` and the corresponding D3D8 interface resolve to the same facade
identity. Other IIDs fail closed, even if the native runtime supports them.
The device retains the factory facade until its own final release, and
`GetDirect3D` returns that facade after calling native `GetDirect3D` and
releasing its returned reference. This assumes the native runtime returns the
creating factory as the device parent; no native identity registry verifies it.
Invalid output arguments and allocation failures can differ from native COM
behavior. Callers must hold valid references; a raw pointer is not safe when
another thread performs its final `Release`.

All other factory and device methods forward to native objects and preserve
native resource pointers. This does not establish general D3D8 fidelity,
complete COM identity, Wine/Windows parity, or gameplay behavior.

Run the fake contract and i686 SDK signature checks from `t3sdk`:

```
python3 -B -m unittest discover -s t3render/tests -p test_d3d8_device_facade.py -v
```
