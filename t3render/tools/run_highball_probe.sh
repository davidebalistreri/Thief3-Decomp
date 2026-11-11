#!/bin/sh
# Run the x86 D3D12 probe with the verified Highball renderer search path.
# This changes only the child environment, not Highball's saved settings.
set -u

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <engine-root> <bottle-root> <x86-probe.exe> <absolute-output-prefix>" >&2
    exit 2
fi
engine=$1
bottle=$2
probe=$3
prefix=$4
for path in "$engine" "$bottle" "$probe" "$prefix"; do
    case "$path" in
        /*) ;;
        *) echo "All paths must be absolute: $path" >&2; exit 2 ;;
    esac
    case "$path" in
        *:*) echo "Paths containing ':' cannot be used in Wine search paths: $path" >&2; exit 2 ;;
    esac
done

wine=$engine/engine/bin/wine
wineserver=$engine/engine/bin/wineserver
if [ ! -x "$wine" ] || [ ! -x "$wineserver" ] || [ ! -d "$engine/frameworks" ]; then
    echo "Invalid Highball engine: $engine" >&2
    exit 2
fi
if [ ! -d "$bottle" ] || [ ! -L "$bottle/dosdevices/z:" ] ||
        [ "$(readlink "$bottle/dosdevices/z:")" != / ]; then
    echo "Bottle must have a Z: drive mapped to /: $bottle" >&2
    exit 2
fi
if [ ! -f "$probe" ]; then
    echo "Probe not found: $probe" >&2
    exit 2
fi
probe_type=$(LC_ALL=C file -b "$probe")
case "$probe_type" in
    *"(DLL)"*) echo "Probe must be an executable, not a DLL: $probe" >&2; exit 2 ;;
    "PE32 executable"*"Intel 80386"*) ;;
    *) echo "Probe must be a 32-bit x86 PE executable: $probe" >&2; exit 2 ;;
esac

log=$prefix.log
image=$prefix.ppm
if [ -e "$image" ] || [ -L "$image" ] || [ -e "$log" ] || [ -L "$log" ]; then
    echo "Output already exists: $log or $image" >&2
    exit 1
fi
# Open the log exclusively and retain the descriptor; the probe itself uses
# CREATE_NEW for the PPM, so an existing image is never overwritten.
set -C
if exec 3>"$log"; then :; else
    echo "Cannot create log: $log" >&2
    exit 1
fi
set +C

windows_image=Z:$(printf '%s' "$image" | sed 's,/,\\,g')
wine_dll_path=$bottle/.highball-dlss/wine:$engine/renderers/d3dmetal-tsshim/wine:$engine/renderers/d3dmetal/wine
frameworks=$engine/frameworks
printf 'T3Render x86 D3D12 probe\nengine=%s\nbottle=%s\nprobe=%s\nimage=%s\n' \
    "$engine" "$bottle" "$probe" "$image" >&3
printf 'WINEPREFIX=%s\nWINESERVER=%s\nWINEDLLPATH_PREPEND=%s\n' "$bottle" "$wineserver" "$wine_dll_path" >&3
printf 'DYLD_FALLBACK_LIBRARY_PATH=%s\nDYLD_FALLBACK_FRAMEWORK_PATH=%s\n' \
    "$frameworks:$frameworks/GStreamer.framework/Versions/1.0/lib" "$frameworks" >&3
printf 'WINEDLLOVERRIDES=%s\nWINEDEBUG=%s\nwindows_output=%s\n' \
    'nvapi64,nvngx=n,b;winemenubuilder.exe=d' '-all,+loaddll' "$windows_image" >&3

# Do not inherit extra library paths which could silently reintroduce DXMT.
# Other user environment, including Wine's normal HOME/PATH, is preserved.
if env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH \
    WINEPREFIX="$bottle" WINESERVER="$wineserver" \
    WINEDLLPATH_PREPEND="$wine_dll_path" \
    DYLD_FALLBACK_LIBRARY_PATH="$frameworks:$frameworks/GStreamer.framework/Versions/1.0/lib" \
    DYLD_FALLBACK_FRAMEWORK_PATH="$frameworks" \
    WINEDLLOVERRIDES='nvapi64,nvngx=n,b;winemenubuilder.exe=d' \
    WINEDEBUG='-all,+loaddll' \
    "$wine" "$probe" --output "$windows_image" >&3 2>&1; then
    status=0
else
    status=$?
fi
printf 'wine_exit_status=%s\n' "$status" >&3
exec 3>&-
echo "Log: $log"
if [ -f "$image" ]; then echo "Image: $image"; fi
exit "$status"
