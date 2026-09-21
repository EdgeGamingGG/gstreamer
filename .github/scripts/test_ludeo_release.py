import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("release", Path(__file__).with_name("ludeo_release.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.previous = Path.cwd()
        os.chdir(self.temp.name)
        self.addCleanup(self.restore)
        self.git("init", "-q")
        self.git("config", "user.name", "Release test")
        self.git("config", "user.email", "release@example.invalid")
        Path("meson.build").write_text("project('gstreamer-full', 'c', version : '1.26.11.1')\n")
        self.commit()
        upstream = self.git("rev-parse", "HEAD")
        self.git("tag", "1.26.11")
        self.metadata = {"upstream_release": "1.26.11", "upstream_commit": upstream, "meson_version": "1.26.11.1"}
        self.write_metadata()
        self.commit()
        self.sha = self.git("rev-parse", "HEAD")

    def restore(self):
        os.chdir(self.previous)
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.check_output(["git", *args], text=True, stderr=subprocess.DEVNULL).strip()

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")

    def write_metadata(self):
        Path("ludeo-release.json").write_text(json.dumps(self.metadata))

    def test_initial_release_and_same_commit_retry(self):
        self.assertEqual(release.prepare("1.0.0", self.sha)["tag"], "1.26.11-ludeo.1.0.0")
        self.git("tag", "-a", "1.26.11-ludeo.1.0.0", "-m", "release")
        self.assertEqual(release.prepare("1.0.0", self.sha)["commit"], self.sha)

    def test_reject_malformed_versions_and_commit(self):
        for value in ["01.0.0", "1.0", "1.0.0-rc.1", "1.0.0\n", "$(id)"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                release.prepare(value, self.sha)
        with self.assertRaises(ValueError):
            release.prepare("1.0.0", "0" * 40)

    def test_existing_tag_cannot_move(self):
        self.git("tag", "1.26.11-ludeo.1.0.0", self.metadata["upstream_commit"])
        with self.assertRaisesRegex(ValueError, "another commit"):
            release.prepare("1.0.0", self.sha)

    def test_versions_compare_numerically(self):
        self.git("tag", "1.26.11-ludeo.1.9.0")
        release.prepare("1.10.0", self.sha)
        with self.assertRaisesRegex(ValueError, "older"):
            release.prepare("1.8.0", self.sha)

    def test_meson_metadata_must_match(self):
        self.metadata["meson_version"] = "1.26.11"
        self.write_metadata()
        self.commit()
        with self.assertRaisesRegex(ValueError, "Meson version"):
            release.prepare("1.0.0", self.git("rev-parse", "HEAD"))

    def test_dirty_source_cannot_release(self):
        Path("meson.build").write_text("modified")
        with self.assertRaisesRegex(ValueError, "committed"):
            release.prepare("1.0.0", self.sha)

    def test_upstream_must_be_an_ancestor(self):
        self.git("checkout", "--orphan", "unrelated")
        self.commit()
        unrelated = self.git("rev-parse", "HEAD")
        self.git("checkout", "--detach", self.sha)
        self.metadata["upstream_commit"] = unrelated
        self.write_metadata()
        self.commit()
        with self.assertRaises(subprocess.CalledProcessError):
            release.prepare("1.0.0", self.git("rev-parse", "HEAD"))


if __name__ == "__main__":
    unittest.main()
