"""Argument validation and --dry-run of release.sh (python3 -m unittest discover -s tests).

Each test copies release.sh into a throwaway git repo with a local bare "origin", so nothing
here can tag, commit to or push the real repository. Only --dry-run and the argument errors
(which exit before any git write or gh call) are exercised.
"""

import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "release.sh")

GIT_ENV = {
    "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
    "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
}


@unittest.skipUnless(shutil.which("git") and shutil.which("bash"), "needs git and bash")
class ReleaseShTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="release_sh_test_")
        self.repo = os.path.join(self.tmp, "repo")
        origin = os.path.join(self.tmp, "origin.git")
        self.env = dict(os.environ, **GIT_ENV, NO_COLOR="1", WATCH="0")
        self.env["PATH"] = os.environ.get("PATH", "")
        self.git("init", "-q", "--bare", origin, cwd=self.tmp)
        os.makedirs(self.repo)
        self.git("init", "-q")
        shutil.copy(SCRIPT, os.path.join(self.repo, "release.sh"))
        self.write_version("0.1.0")
        self.git("add", "-A")
        self.git("commit", "-qm", "init")
        self.git("remote", "add", "origin", origin)
        self.git("push", "-q", "origin", "HEAD")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def git(self, *args, cwd=None):
        return subprocess.run(["git", *args], cwd=cwd or self.repo, env=getattr(self, "env", None) or
                              dict(os.environ, **GIT_ENV), check=True, capture_output=True, text=True).stdout

    def write_version(self, v):
        with open(os.path.join(self.repo, "VERSION"), "w") as f:
            f.write(v + "\n")

    def commit_version(self, v):
        self.write_version(v)
        self.git("commit", "-qam", "v" + v)

    def run_release(self, *args):
        return subprocess.run(["bash", "release.sh", *args], cwd=self.repo, env=self.env,
                              capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60)

    def assert_untouched(self, version="0.1.0"):
        self.assertEqual(self.git("tag").strip(), "")
        with open(os.path.join(self.repo, "VERSION")) as f:
            self.assertEqual(f.read().strip(), version)
        self.assertEqual(self.git("status", "--porcelain").strip(), "")

    def test_dry_run_beta(self):
        r = self.run_release("--dry-run", "0.1.0-beta.1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("New version:     0.1.0-beta.1 (explicit)", r.stdout)
        self.assertIn("pre-release (GitHub release --prerelease (not marked latest))", r.stdout)
        self.assertIn("git tag v0.1.0-beta.1", r.stdout)
        self.assertIn("write VERSION: 0.1.0 -> 0.1.0-beta.1", r.stdout)
        self.assertIn("--channel beta", r.stdout)
        self.assert_untouched()

    def test_dry_run_rc_with_v_prefix(self):
        r = self.run_release("v1.2.3-rc.2", "--dry-run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("git tag v1.2.3-rc.2", r.stdout)
        self.assertIn("pre-release", r.stdout)
        self.assert_untouched()

    def test_dry_run_stable_is_latest(self):
        for arg, want in (("0.2.0", "0.2.0"), ("patch", "0.1.1"), ("minor", "0.2.0"), ("major", "1.0.0")):
            r = self.run_release("--dry-run", arg)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("New version:     %s" % want, r.stdout)
            self.assertIn("stable (GitHub release --latest)", r.stdout)
            self.assertNotIn("--prerelease", r.stdout)
        self.assert_untouched()

    def test_dry_run_current_version(self):
        r = self.run_release("--dry-run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("VERSION already 0.1.0; no commit", r.stdout)
        self.assertIn("git tag v0.1.0", r.stdout)
        self.assert_untouched()

    def test_invalid_versions(self):
        for bad in ("1.2", "1.2.3.4", "01.2.3", "1.2.3-beta", "1.2.3-beta.0", "1.2.3-beta.01",
                    "1.2.3-alpha.1", "1.2.3-BETA.1", "1.2.3-beta.1.2", "1.2.3+build", "latest", "beta"):
            r = self.run_release("--dry-run", bad)
            self.assertNotEqual(r.returncode, 0, bad)
            self.assertIn("Invalid version", r.stderr, bad)
        self.assert_untouched()

    def test_bad_options(self):
        r = self.run_release("--dry-run", "--force")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("Unknown option", r.stderr)
        r = self.run_release("--dry-run", "0.1.1", "0.1.2")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("Only one version argument", r.stderr)
        self.assert_untouched()

    def test_bump_refused_on_prerelease(self):
        self.commit_version("0.1.0-beta.2")
        r = self.run_release("--dry-run", "patch")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("pre-release", r.stderr)
        self.assertIn("./release.sh 0.1.0", r.stderr)
        # Finishing the pre-release by name works.
        r = self.run_release("--dry-run", "0.1.0")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("write VERSION: 0.1.0-beta.2 -> 0.1.0", r.stdout)
        self.assert_untouched("0.1.0-beta.2")

    def test_invalid_version_file(self):
        self.commit_version("0.1")
        r = self.run_release("--dry-run", "0.1.0")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("Invalid VERSION file", r.stderr)

    def test_existing_tag_refused(self):
        self.git("tag", "v0.1.0-beta.1")
        r = self.run_release("--dry-run", "0.1.0-beta.1")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("already exists", r.stderr)

    def test_remote_tag_refused(self):
        self.git("tag", "v0.1.0-rc.1")
        self.git("push", "-q", "origin", "v0.1.0-rc.1")
        self.git("tag", "-d", "v0.1.0-rc.1")
        r = self.run_release("--dry-run", "0.1.0-rc.1")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("Remote tag v0.1.0-rc.1 already exists", r.stderr)

    def test_dirty_tree_refused(self):
        with open(os.path.join(self.repo, "release.sh"), "a") as f:
            f.write("\n# local change\n")
        r = self.run_release("--dry-run", "0.1.0-beta.1")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not clean", r.stderr)

    def test_help(self):
        r = self.run_release("--help")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("X.Y.Z-beta.N", r.stdout)
        self.assertIn("--dry-run", r.stdout)


if __name__ == "__main__":
    unittest.main()
