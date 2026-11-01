import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BuildScriptTests(unittest.TestCase):
    def test_symlinked_core_with_spaces_gets_an_isolated_space_free_default(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            physical_core = home / "External Drive" / "platformio"
            physical_core.mkdir(parents=True)
            (home / ".platformio-p4").symlink_to(physical_core, target_is_directory=True)
            bin_dir = home / "bin"
            bin_dir.mkdir()
            pio = bin_dir / "pio"
            pio.write_text(
                '#!/bin/sh\nprintf "%s\\n%s\\n%s\\n" "$PLATFORMIO_CORE_DIR" '
                '"$PLATFORMIO_BUILD_DIR" "$*" > "$TEST_CAPTURE"\n'
            )
            pio.chmod(0o755)
            capture = home / "invocation"
            env = dict(os.environ, HOME=str(home), TEST_CAPTURE=str(capture),
                       PATH=str(bin_dir) + os.pathsep + os.environ["PATH"])
            for name in ("PLATFORMIO_CORE_DIR", "PLATFORMIO_BUILD_DIR", "ZELLO_PIO_CORE_DIR",
                         "ZELLO_P4_PIO_CORE_DIR", "ZELLO_BUILD_DIR"):
                env.pop(name, None)
            command = ["bash", str(ROOT / "scripts/build-upload-monitor.sh"), "--wio-tracker-l2", "-B"]
            result = subprocess.run(command, env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            core, build, args = capture.read_text().splitlines()
            self.assertNotIn(" ", str(Path(core).resolve()))
            self.assertNotIn(" ", str(Path(build).resolve()))
            self.assertEqual(args, "run -e wio-tracker-l2")
            self.assertTrue((home / ".platformio-p4").is_symlink())

            capture.unlink()
            env["ZELLO_PIO_CORE_DIR"] = str(home / ".platformio-p4")
            result = subprocess.run(command, env=env, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(capture.exists())

            explicit_core = home / "explicit-core"
            env["ZELLO_PIO_CORE_DIR"] = str(explicit_core)
            result = subprocess.run(command, env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(Path(capture.read_text().splitlines()[0]), explicit_core.resolve())

            capture.unlink()
            env["ZELLO_BUILD_DIR"] = str(home / "Other Drive" / "build")
            result = subprocess.run(command, env=env, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(capture.exists())


if __name__ == "__main__":
    unittest.main()
