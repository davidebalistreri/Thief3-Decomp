# Isolated D3D8 factory facade

This fixture-only component is incomplete: `CreateDevice` returns
`E_NOTIMPL` without calling the native factory. No real application or TDS can
use it yet. It is not integrated into the D3D8 proxy or build configuration.

`Adopt` consumes one native factory reference on success and leaves it with
the caller on failure. The fixture must adopt a given native identity only
once; this component does not connect to an identity registry. Supported
`IUnknown` and `IDirect3D8` queries share one facade identity. All other IIDs
fail closed, even if the native object supports them, so compatibility with
extensions is not claimed. Other factory methods forward directly.

Factory reference counting prevents incrementing a zero count and retains one
native reference while any valid facade reference remains. COM callers must
hold a reference while calling it: this component does not make a raw pointer
safe against a concurrent final `Release`.

There is no device facade and no resource wrappers. A future partial device
facade that returns native resources would still expose the native device
through resource `GetDevice`; it is unsafe to combine that escape with wrapped
buffers. That boundary must be closed before testing such a configuration in
TDS.

Run the host contract and i686 SDK signature checks with:

```
python3 -B -m unittest discover -s t3render/tests -p test_d3d8_factory_facade.py -v
```

The isolated x86 smoke test passed on 2026-11-12 using Highball CrossOver
26.3 r30 with Wine builtin D3D8 on Apple M5 Pro. It compared native and facade
factory results, checked the COM identity and rejected IID, verified that
`CreateDevice` returned `E_NOTIMPL`, and released the facade and native refs.
It saw one adapter and 88 modes. No device or TDS process was started.

To repeat, set the engine and an available bottle, then run from `t3sdk`:

```sh
T3RENDER_ENGINE='/absolute/path/to/Highball/engine'
T3RENDER_BOTTLE='/absolute/path/to/Highball/bottle'
T3RENDER_TMP=$(mktemp -d /private/tmp/t3render-factory-smoke.XXXXXX)
i686-w64-mingw32-g++ -std=c++17 -Wall -Wextra -Werror -static -static-libgcc -static-libstdc++ \
  t3render/tests/d3d8_factory_facade_wine.cpp -ld3d8 -ldxguid -o "$T3RENDER_TMP/factory-smoke.exe"
env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH \
  WINEPREFIX="$T3RENDER_BOTTLE" WINESERVER="$T3RENDER_ENGINE/engine/bin/wineserver" \
  WINEDLLPATH_PREPEND="$T3RENDER_BOTTLE/.highball-dlss/wine:$T3RENDER_ENGINE/renderers/d3dmetal-tsshim/wine:$T3RENDER_ENGINE/renderers/d3dmetal/wine" \
  DYLD_FALLBACK_LIBRARY_PATH="$T3RENDER_ENGINE/frameworks:$T3RENDER_ENGINE/frameworks/GStreamer.framework/Versions/1.0/lib" \
  DYLD_FALLBACK_FRAMEWORK_PATH="$T3RENDER_ENGINE/frameworks" WINEDEBUG=-all \
  WINEDLLOVERRIDES='d3d8=b;nvapi64,nvngx=n,b;winemenubuilder.exe=d' \
  "$T3RENDER_ENGINE/engine/bin/wine" "$T3RENDER_TMP/factory-smoke.exe"
rm -r "$T3RENDER_TMP"
```

Wine may update its bottle state. The result applies only to this selected
runtime and factory calls; it does not validate gameplay or a device facade.
