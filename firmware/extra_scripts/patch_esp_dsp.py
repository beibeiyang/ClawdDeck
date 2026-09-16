"""Limit esp-dsp to core modules — skip demo apps that break PlatformIO builds."""
Import("env")
import json
from pathlib import Path

lib = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "esp-dsp"
if not lib.is_dir():
    print("patch_esp_dsp: esp-dsp not installed yet")
else:
    cfg = {
        "name": "esp-dsp",
        "version": "1.6.0",
        "build": {
            "includeDir": "modules/common/include",
            "flags": [
                "-I modules/fft/include",
                "-I modules/common/include",
                "-I modules/dotprod/include",
                "-I modules/support/include",
                "-I modules/windows/include",
            ],
            "srcFilter": [
                "+<modules/common/>",
                "+<modules/fft/>",
                "-<modules/fft/test/>",
                "-<modules/fft/test_sim/>",
                "+<modules/windows/>",
                "-<modules/windows/test/>",
                "+<modules/dotprod/>",
                "-<modules/dotprod/test/>",
                "+<modules/support/>",
                "-<modules/support/sfdr/test/>",
                "-<modules/support/snr/test/>",
                "-<modules/support/cplx_gen/test/>",
                "-<modules/support/view/test/>",
                "-<modules/support/mem/test/>"
            ],
        },
    }
    (lib / "library.json").write_text(json.dumps(cfg, indent=2) + "\n")
    print("patch_esp_dsp: filtered esp-dsp to modules/ only")
