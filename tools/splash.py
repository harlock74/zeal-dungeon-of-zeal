#!/usr/bin/env python3
"""Pack two SDK title layers into the existing streamed ZTR1 container."""
from pathlib import Path
import sys
import xml.etree.ElementTree as ElementTree

sys.dont_write_bytecode = True
from validate_assets import read_splash, splash_error

PROJECT_ROOT = Path(__file__).resolve().parents[1]
SDK_MAPS_DIR = PROJECT_ROOT / "build/maps"
RUNTIME_FILE = PROJECT_ROOT / "build/runtime/assets/splash.ztr"


def main():
    root, layers, value_records = read_splash()
    if "--check" not in sys.argv:
        converted_layers = []
        for layer_index, gids in enumerate(layers):
            sdk_map = SDK_MAPS_DIR / f"splash-layer{layer_index}.ztm"
            converted = sdk_map.read_bytes()
            expected = bytes(gid - 1 if gid else 255 for gid in gids)
            if converted != expected:
                splash_error("SDK conversion differs from validated title layer")
            converted_layers.append(bytes(gid - 1 if gid else 0 for gid in gids))
        # Existing layout: header, scene,125 collision bytes, two value records,
        # then lettering. Both layers still reuse the same1200-byte game buffer.
        header = b"ZTR1" + bytes([2, 255, 255, 2])
        body = (header + converted_layers[0] + bytes(125)
                + b"".join(value_records) + converted_layers[1])
        RUNTIME_FILE.parent.mkdir(parents=True, exist_ok=True)
        RUNTIME_FILE.write_bytes(body)
    print("Splash valid: two40x30 layers; shared zdungeon.tsx; "
          "both dynamic-value placeholders present.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, ElementTree.ParseError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
