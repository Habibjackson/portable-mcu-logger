Import("env")

import sys
from pathlib import Path

sys.path.insert(0, str(Path(env["PROJECT_DIR"]) / "tools"))
from verify_flash_layout import verify


def check_logger_layout(target, source, env):
    profiles = {"genericSTM32F103RD": "rd", "bluepill_f103c8": "c8"}
    profile = profiles[env["PIOENV"]]
    result = verify(Path(str(target[0])), profile)
    print("Flash layout verified: firmware ends by "
          f"{result['firmware_loaded_end']:#010x}; logger "
          f"{result['log_start']:#010x}..{result['log_end']:#010x}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf",
                  env.VerboseAction(check_logger_layout,
                                    "Verifying logger flash layout"))
