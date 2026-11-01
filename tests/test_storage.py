"""Host regressions for the real settings, channel, XML and YAML C sources."""

import os
from pathlib import Path
import shutil
import subprocess
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]


class StorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = ROOT / "tests" / "storage" / (".host-build-" + uuid.uuid4().hex)
        cls.build.mkdir()
        cls.addClassCleanup(shutil.rmtree, cls.build)
        cls.binary = cls.build / "storage-tests"
        sources = ["settings.c", "channel_list.c", "xml_util.c", "yaml_util.c", "file_store.c"]
        command = [
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-D_GNU_SOURCE", "-I", str(ROOT / "tests" / "storage" / "include"),
            "-I", str(ROOT / "main"),
            "-include", str(ROOT / "tests" / "storage" / "host.h"),
            *[str(ROOT / "main" / name) for name in sources],
            str(ROOT / "tests" / "storage" / "host.c"),
            str(ROOT / "tests" / "storage" / "storage_tests.c"),
            "-o", str(cls.binary),
        ]
        if os.environ.get("STORAGE_SANITIZE"):
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        env = dict(os.environ, TMPDIR=str(cls.build))
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, name):
        work = self.build / name
        work.mkdir()
        result = subprocess.run(
            [str(self.binary), name], cwd=work, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_interrupted_file_promotion_recovers_old_data(self):
        self.run_case("recover")

    def test_failed_file_promotion_keeps_old_data(self):
        self.run_case("promote")

    def test_channel_save_failure_preserves_old_file(self):
        self.run_case("channel-save-failure")

    def test_channel_startup_recovers_backup_not_partial_stage(self):
        self.run_case("channel-recovery")

    def test_malformed_channel_import_does_not_replace_list(self):
        self.run_case("malformed-import")

    def test_channel_entry_body_cannot_be_silently_discarded(self):
        self.run_case("channel-body")

    def test_import_limits_and_invalid_entries_do_not_truncate(self):
        self.run_case("import-limits")

    def test_invalid_xml_text_cannot_replace_saved_channels(self):
        self.run_case("invalid-channel-text")

    def test_complete_xml_document_validation(self):
        self.run_case("xml-document")

    def test_xml_attributes_are_not_found_inside_other_values(self):
        self.run_case("xml-attributes")

    def test_supported_xml_import_retains_exact_identifiers(self):
        self.run_case("valid-import")

    def test_channel_names_are_case_sensitive(self):
        self.run_case("exact-names")

    def test_rename_collision_preserves_both_channels(self):
        self.run_case("rename-collision")

    def test_maximum_escaped_channel_list_reloads(self):
        self.run_case("large-roundtrip")

    def test_yaml_empty_collection_suffix_is_rejected(self):
        self.run_case("yaml-suffix")

    def test_yaml_completed_flow_collections_reject_children(self):
        self.run_case("yaml-completed-flow")

    def test_yaml_block_and_empty_sections_remain_supported(self):
        self.run_case("yaml-sections")

    def test_yaml_supported_empty_and_quoted_values(self):
        self.run_case("yaml-valid")

    def test_settings_numeric_bounds(self):
        self.run_case("numeric-bounds")

    def test_failed_settings_save_does_not_mix_credentials(self):
        self.run_case("snapshot-failure")

    def test_legacy_settings_migrate_without_loss(self):
        self.run_case("migration")

    def test_legacy_numeric_values_migrate_with_credentials_preserved(self):
        for case in ("legacy-numeric-migration", "legacy-numeric-max"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_failed_migration_retains_legacy_settings_for_retry(self):
        self.run_case("migration-failure")

    def test_invalid_legacy_settings_are_not_silently_defaulted(self):
        self.run_case("invalid-legacy")

    def test_invalid_snapshot_is_not_silently_reset(self):
        self.run_case("invalid-snapshot")

    def test_nvs_initialization_failure_does_not_erase_settings(self):
        self.run_case("nvs-init-failure")


if __name__ == "__main__":
    unittest.main()
