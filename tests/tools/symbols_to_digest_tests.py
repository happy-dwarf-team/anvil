"""Tests of tools/symbols-to-digest.py, run by CTest (anvil_symbols_to_digest_tests).

Usage: python -X utf8 -I tests/tools/symbols_to_digest_tests.py <tools/symbols-to-digest.py>

The bytes below are invented, not the game's code.
"""
import hashlib
import subprocess
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

TOOL = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path(__file__).parents[2] / "tools/symbols-to-digest.py"


def digest(pattern):
    tokens = pattern.split()
    return hashlib.sha256(bytes(0 if t == "??" else int(t, 16) for t in tokens)).hexdigest().upper()


class SymbolsToDigest(unittest.TestCase):
    def run_tool(self, text):
        folder = Path(tempfile.mkdtemp())
        path = folder / "symbols.toml"
        path.write_text(text, encoding="utf-8", newline="")
        result = subprocess.run([sys.executable, "-X", "utf8", "-I", str(TOOL), str(path)],
                                capture_output=True, text=True)
        return result, path.read_text(encoding="utf-8")

    def test_basic_and_literal_strings_become_digests(self):
        result, out = self.run_tool(
            'build = "eu4-1.37.5-steam-win64"\n\n'
            '[symbol.basic]\nrva = 0x1000\nbytes = "40 53 ?? 90 90"\nmemory = "code"\n\n'
            "[symbol.literal]\nrva = 0x2000\nbytes = '40 53 ?? 90 90'\nmemory = \"code\"\n\n"
            "[symbol.tight]\nrva = 0x3000\nbytes='48 8B 05 ?? ?? ?? ??'\nmemory = \"code\"\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        symbols = tomllib.loads(out)["symbol"]
        for name in ("basic", "literal"):
            self.assertNotIn("bytes", symbols[name])
            self.assertEqual(symbols[name]["sha256"], digest("40 53 ?? 90 90"))
            self.assertEqual((symbols[name]["size"], symbols[name]["any"]), (5, [2]))
        self.assertEqual(symbols["tight"]["sha256"], digest("48 8B 05 ?? ?? ?? ??"))
        self.assertEqual(symbols["tight"]["any"], [3, 4, 5, 6])

    def test_multiline_basic_and_literal_strings_become_digests(self):
        result, out = self.run_tool(
            '[symbol.basic]\nrva = 0x1000\nbytes = """\n40 53\n?? 90\n"""\nmemory = "code"\n\n'
            "[symbol.literal]\nrva = 0x2000\nbytes = '''\n40 53\n?? 90\n'''\nmemory = \"code\"\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        symbols = tomllib.loads(out)["symbol"]
        for name in ("basic", "literal"):
            self.assertNotIn("bytes", symbols[name])
            self.assertEqual(symbols[name]["sha256"], digest("40 53 ?? 90"))
            self.assertEqual(symbols[name]["any"], [2])

    def test_per_build_literal_strings_and_a_value_get_is(self):
        result, out = self.run_tool(
            'builds = ["eu4-1.37.5-steam-win64", "eu4-1.37.5.2-gog-win64"]\n\n'
            '[symbol.site]\nmemory = "code"\n'
            'values.offset = { at = 3, size = 4 }\n'
            "build.\"eu4-1.37.5-steam-win64\" = { rva = 0x1000, bytes = 'C7 C1 F4 18 04 00 00' }\n"
            'build."eu4-1.37.5.2-gog-win64" = { rva = 0x2000, bytes = "C7 C1 F4 18 04 00 00" }\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        site = tomllib.loads(out)["symbol"]["site"]
        for build in site["build"].values():
            self.assertNotIn("bytes", build)
            self.assertEqual(build["sha256"], digest("C7 C1 F4 18 04 00 00"))
        self.assertEqual(site["values"]["offset"]["is"], 0x418)

    def test_bytes_it_cannot_read_leave_the_file_and_fail(self):
        text = '[symbol.odd]\nrva = 0x1000\nbytes = "40 53 G0"\nmemory = "code"\n'
        result, out = self.run_tool(text)
        self.assertEqual(result.returncode, 1)
        self.assertIn("odd", result.stderr)
        self.assertEqual(out, text)


if __name__ == "__main__":
    unittest.main()
