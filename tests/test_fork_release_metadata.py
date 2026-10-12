import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/ci/fork-release-metadata.py"


class ForkReleaseMetadataTests(unittest.TestCase):
    def test_sources_checksums_and_draft_resume(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            env = dict(os.environ, GITHUB_SERVER_URL="https://github.com", GITHUB_REPOSITORY="zhuoyiwan/msime-windows",
                       GITHUB_RUN_ID="123", GITHUB_SHA="a" * 40, SOURCE_SHA="b" * 40)
            def run(*args, success=True):
                result = subprocess.run([sys.executable, str(SCRIPT), *args, "--directory", str(root)],
                                        env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, success, result.stderr)
                return result.stdout
            for platform, extension in [("windows", "exe"), ("android", "apk"), ("macos", "dmg")]:
                asset = f"synthetic-{platform}.{extension}"
                (root / asset).write_bytes(platform.encode())
                extra = ["--certificate", "2cc60b29966625bc9834128a71bd1d7702d3137f7bd791d5093b9b68197a3385", "--version-code", "3006"] if platform == "android" else []
                run("record", "--platform", platform, "--source", env["GITHUB_SHA" if platform == "windows" else "SOURCE_SHA"],
                    "--asset", asset, "--version", "1.0.0", *extra)
            run("assemble")
            manifest = json.loads((root / "release-provenance.json").read_text())
            self.assertEqual(manifest["windows"]["source_commit"], env["GITHUB_SHA"])
            self.assertEqual(manifest["android"]["versionCode"], 3006)
            release_path = root.parent / (root.name + "-release.json")
            try:
                asset = root / "synthetic-android.apk"
                release = {"draft": True, "assets": [{"name": asset.name, "size": asset.stat().st_size,
                    "digest": "sha256:" + hashlib.sha256(asset.read_bytes()).hexdigest()}]}
                release_path.write_text(json.dumps(release))
                missing = run("resume", "--release-json", str(release_path)).splitlines()
                self.assertNotIn(asset.name, missing)
                self.assertIn("release-provenance.json", missing)
                release["assets"][0]["digest"] = "sha256:" + "0" * 64
                release_path.write_text(json.dumps(release))
                run("resume", "--release-json", str(release_path), success=False)
                release["draft"] = False
                release_path.write_text(json.dumps(release))
                run("resume", "--release-json", str(release_path), success=False)
            finally:
                release_path.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
