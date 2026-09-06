import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("liquid_overlay_provider.py")
SPEC = importlib.util.spec_from_file_location("liquid_overlay_provider", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class BadgeMappingTest(unittest.TestCase):
    def test_maps_all_drive_states(self):
        self.assertEqual(MODULE.badge_for_state("online-only"), "emblem-synchronizing")
        self.assertEqual(MODULE.badge_for_state("available-offline"), "emblem-default")
        self.assertEqual(MODULE.badge_for_state("not-synced-here"), "emblem-unreadable")

    def test_ignores_unknown_state(self):
        self.assertEqual(MODULE.badge_for_state("unknown"), "")

    def test_accepts_file_urls_only(self):
        self.assertEqual(MODULE.local_path("file:///tmp/drive%20file"), "/tmp/drive file")
        self.assertIsNone(MODULE.local_path("https://example.invalid/file"))


if __name__ == "__main__":
    unittest.main()
