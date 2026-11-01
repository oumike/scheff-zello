"""Host regression checks for the firmware's web configuration."""

import os
import pathlib
import shlex
import shutil
import subprocess
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]


def find_cjson_sources():
    framework = pathlib.Path("packages/framework-espidf/components/json/cJSON")
    candidates = []
    for key, suffix in (
        ("CJSON_DIR", pathlib.Path()),
        ("IDF_PATH", pathlib.Path("components/json/cJSON")),
        ("ZELLO_PIO_CORE_DIR", framework),
        ("PLATFORMIO_CORE_DIR", framework),
    ):
        if os.environ.get(key):
            candidates.append(pathlib.Path(os.environ[key]).expanduser() / suffix)
    home = pathlib.Path.home()
    candidates.extend(core / framework for core in (
        home / ".cache/scheff-zello/platformio", home / ".platformio-p4", home / ".platformio",
    ))
    return next((path.resolve() for path in candidates if (path / "cJSON.c").is_file()), None)


class SourceDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.workspace = ROOT / "tests/web/build" / f"discovery-{os.getpid()}-{self._testMethodName}"
        self.workspace.mkdir(parents=True, exist_ok=False)
        self.addCleanup(shutil.rmtree, self.workspace)
        self.home = self.workspace / "home"
        home = mock.patch.object(pathlib.Path, "home", return_value=self.home)
        home.start()
        self.addCleanup(home.stop)
        environment = mock.patch.dict(os.environ, {}, clear=True)
        environment.start()
        self.addCleanup(environment.stop)

    def source_at(self, path):
        path.mkdir(parents=True, exist_ok=True)
        (path / "cJSON.c").write_text("/* Discovery fixture; not compiled. */\n")
        return path

    def core_at(self, path):
        return self.source_at(path / "packages/framework-espidf/components/json/cJSON")

    def test_build_cache_without_legacy_install(self):
        expected = self.core_at(self.home / ".cache/scheff-zello/platformio")
        self.assertEqual(find_cjson_sources(), expected)

    def test_explicit_core_overrides_precede_default_installs(self):
        self.core_at(self.home / ".platformio-p4")
        self.core_at(self.home / ".cache/scheff-zello/platformio")
        for key in ("ZELLO_PIO_CORE_DIR", "PLATFORMIO_CORE_DIR"):
            with self.subTest(variable=key):
                core = self.workspace / key
                expected = self.core_at(core)
                with mock.patch.dict(os.environ, {key: str(core)}):
                    self.assertEqual(find_cjson_sources(), expected)

    def test_explicit_sources_keep_precedence(self):
        expected = self.source_at(self.workspace / "explicit-cjson")
        idf = self.workspace / "explicit-idf"
        idf_source = self.source_at(idf / "components/json/cJSON")
        self.core_at(self.home / ".cache/scheff-zello/platformio")
        with mock.patch.dict(os.environ, {"CJSON_DIR": str(expected), "IDF_PATH": str(idf)}):
            self.assertEqual(find_cjson_sources(), expected)
        with mock.patch.dict(os.environ, {"IDF_PATH": str(idf)}):
            self.assertEqual(find_cjson_sources(), idf_source)


class BrowserTests(unittest.TestCase):
    def test_browser_behavior(self):
        result = subprocess.run(
            ["node", "--test", "tests/web/browser.test.cjs"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class HandlerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cjson = find_cjson_sources()
        if cjson is None:
            raise RuntimeError("Set CJSON_DIR, IDF_PATH, or ZELLO_PIO_CORE_DIR to the installed firmware cJSON sources.")
        cls.build = ROOT / "tests/web/build" / str(os.getpid())
        cls.build.mkdir(parents=True, exist_ok=False)
        cls.addClassCleanup(shutil.rmtree, cls.build)
        cls.binary = cls.build / "handlers"
        sources = [
            ROOT / "tests/web/handlers.c",
            ROOT / "main/xml_util.c",
            ROOT / "main/yaml_util.c",
            ROOT / "main/file_store.c",
            cjson / "cJSON.c",
        ]
        command = [
            os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
            *shlex.split(os.environ.get("CFLAGS", "")),
            "-I", str(ROOT / "tests/web/include"), "-I", str(cjson),
            "-include", str(ROOT / "tests/web/include/compat.h"),
            *map(str, sources), "-o", str(cls.binary),
        ]
        result = subprocess.run(
            command, cwd=ROOT, capture_output=True, text=True, timeout=60,
            env={**os.environ, "TMPDIR": str(cls.build)},
        )
        if result.returncode:
            raise AssertionError("Native test compilation failed:\n" + result.stdout + result.stderr)

    def test_handler_behavior(self):
        cases = [
            "json-complete", "json-nul", "json-no-mutation",
            "json-timeout", "upload-timeout", "json-trickle", "upload-trickle",
            "body-lengths", "upload-nul", "body-fragmented",
            "identifier-save", "identifier-orig", "identifier-desc",
            "identifier-delete", "identifier-favorite", "identifier-join",
            "invalid-types", "exact-names", "query-validation", "query-utf8", "current-identity",
            "xml-settings", "xml-empty-channels", "xml-malformed", "xml-quoted-settings", "xml-large-valid",
            "yaml-capacity-one", "yaml-capacity-full", "yaml-overflow", "yaml-overflow-invalid",
            "sd-failure-malloc", "sd-failure-open", "sd-failure-write",
            "sd-failure-flush", "sd-failure-close", "sd-success",
            "sd-recover-read", "sd-recover-export", "sd-nul",
            "sd-import-body-timeout", "sd-export-body-timeout",
            "sd-import-body-trickle", "sd-export-body-trickle",
            "sd-import-body-oversize", "sd-export-body-oversize",
            "sd-import-body-invalid", "sd-export-body-invalid",
            "sd-import-supported", "sd-export-supported",
            "get-unexpected-body", "get-empty-body", "crash-delete-timeout", "crash-delete-supported",
        ]
        for case in cases:
            with self.subTest(case=case):
                result = subprocess.run(
                    [str(self.binary), case], cwd=self.build,
                    capture_output=True, text=True, timeout=5,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
