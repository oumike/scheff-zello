import importlib.util
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_site", ROOT / "scripts/build-site.py")
build_site = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_site)


def entry(kind, subtype, offset, size, name):
    return struct.pack("<2sBBII16sI", b"\xaa\x50", kind, subtype, offset, size, name, 0)


class WebPartsTests(unittest.TestCase):
    def test_skips_data_partitions_inside_the_image(self):
        image = bytearray(b"\xff" * 0x18000)
        image[0] = 0xE9
        table = (entry(1, 2, 0x9000, 0x6000, b"nvs") + entry(1, 1, 0xF000, 0x1000, b"phy_init")
                 + entry(0, 0, 0x10000, 0x600000, b"factory") + entry(1, 0x81, 0x610000, 0x400000, b"storage"))
        image[0x8000:0x8000 + len(table)] = table
        image[0x10000] = 0xE9

        parts = build_site.web_parts(bytes(image))

        self.assertEqual([(offset, len(data)) for offset, data in parts],
                         [(0, 0x9000), (0x10000, 0x8000)])
        self.assertEqual(parts[0][1][0x8000:0x8002], b"\xaa\x50")


if __name__ == "__main__":
    unittest.main()
