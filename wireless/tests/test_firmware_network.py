"""Compile and execute production state-machine functions with host-side I/O mocks.

This is software validation, not an ESP-IDF build or an ESP32-S3 hardware test.
Only SDK includes/unrelated functions are omitted; the selected C function bodies
and their state declarations are read verbatim from main/card_network.c.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = (
    "restore_content", "stop_portal", "network_event", "reset_transfer",
    "websocket_event", "start_websocket", "network_task",
)


def production_code(source):
    state = source[source.index("#define WIFI_READY"):source.index("extern const char wifi_portal_start")]
    bodies = []
    for name in FUNCTIONS:
        match = re.search(rf"^static [^\n]+\b{name}\([^\n]*\)\n\{{.*?^\}}", source, re.M | re.S)
        if not match:
            raise AssertionError(f"Cannot locate production function {name}")
        bodies.append(match.group())
    return state + "\n" + "\n\n".join(bodies)


class FirmwareNetwork(unittest.TestCase):
    def test_production_state_machine(self):
        source = (ROOT / "main/card_network.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "card_network_under_test.inc").write_text(production_code(source))
            executable = build / "network_test"
            compile_result = subprocess.run([
                *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-variable", "-Wno-unused-function", "-Wno-sign-compare",
                "-I", str(build), str(Path(__file__).with_name("firmware_network_host.c")),
                "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
