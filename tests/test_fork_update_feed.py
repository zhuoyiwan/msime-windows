import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("fork_feed", Path(__file__).resolve().parents[1] / "scripts/ci/publish-fork-update-feed.py")
FEED = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FEED)


class ForkFeedTests(unittest.TestCase):
    def release(self):
        tag = "zhuoyiwan-20261013-r7"
        names = ["MetasequoiaIME_Setup_v0.7.1-zhuoyiwan-local-20261013-r10-unsigned.exe",
                 "MetasequoiaIME_Android_v0.3.1-zhuoyiwan-arm64-r7-signed.apk"]
        return {"tag_name": tag, "draft": False, "prerelease": True, "assets": [
            {"name": name, "size": 100, "digest": "sha256:" + "a" * 64,
             "browser_download_url": FEED.PREFIX + tag + "/" + name} for name in names]}

    def test_public_assets_keep_digests_and_download_sources(self):
        release = self.release()
        self.assertEqual(FEED.release_entry(release), release)

    def test_drafts_missing_assets_and_foreign_downloads_are_rejected(self):
        for mutate in [lambda r: r.update(draft=True), lambda r: r["assets"].pop(),
                       lambda r: r["assets"][0].update(digest=None),
                       lambda r: r["assets"][0].update(browser_download_url="https://evil.example/x")]:
            release = self.release()
            mutate(release)
            with self.assertRaises(ValueError):
                FEED.release_entry(release)

    def test_two_windows_assets_cannot_replace_android(self):
        release = self.release()
        release["assets"][1] = dict(release["assets"][0])
        with self.assertRaises(ValueError):
            FEED.release_entry(release)
