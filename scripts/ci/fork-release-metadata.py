"""记录并核对同一次 Actions 构建的三端产物；不接受本地预上传包。"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["record", "assemble", "resume"])
    parser.add_argument("--release-json", type=Path)
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--platform", choices=["windows", "android", "macos"])
    parser.add_argument("--source")
    parser.add_argument("--asset")
    parser.add_argument("--version")
    parser.add_argument("--version-code", type=int)
    parser.add_argument("--certificate")
    args = parser.parse_args()
    root = args.directory
    if args.mode == "resume":
        release = json.loads(args.release_json.read_text(encoding="utf-8"))
        assert release["draft"], "只能恢复未发布草稿"
        existing = {item["name"]: item for item in release["assets"]}
        local = {path.name: path for path in root.iterdir() if path.is_file()}
        assert existing.keys() <= local.keys(), "草稿含不属于本次构建的资产"
        for name, item in existing.items():
            path = local[name]
            assert item["size"] == path.stat().st_size and item.get("digest") == "sha256:" + digest(path), "草稿资产与固定来源构建不同"
        for name in sorted(local.keys() - existing.keys()):
            print(name)
        return
    run = f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{os.environ['GITHUB_RUN_ID']}"
    if args.mode == "record":
        assert re.fullmatch(r"[0-9a-f]{40}", args.source or ""), "必须是固定源码提交"
        assert Path(args.asset).name == args.asset, "资产名不能包含目录"
        path = root / args.asset
        item = dict(asset=args.asset, sha256=digest(path), size=path.stat().st_size,
                    source_commit=args.source, workflow_run=run, version=args.version)
        if args.platform == "android":
            assert args.certificate == "2cc60b29966625bc9834128a71bd1d7702d3137f7bd791d5093b9b68197a3385"
            item.update(abi="arm64-v8a", versionCode=args.version_code, certificate_sha256=args.certificate)
        if args.platform == "windows":
            item.update(architectures=["x64", "x86"], signing="unsigned", uiAccess=False)
        if args.platform == "macos":
            item.update(architectures=["arm64", "x86_64"], signing="local-self-signing", notarized=False)
        (root / f"{args.platform}-provenance.json").write_text(json.dumps(item, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return
    manifest = dict(workflow_commit=os.environ["GITHUB_SHA"], workflow_run=run)
    assets = []
    for platform, extension in [("windows", ".exe"), ("android", ".apk"), ("macos", ".dmg")]:
        item = json.loads((root / f"{platform}-provenance.json").read_text(encoding="utf-8"))
        expected_source = os.environ["GITHUB_SHA"] if platform == "windows" else os.environ["SOURCE_SHA"]
        assert item["source_commit"] == expected_source and item["workflow_run"] == run
        path = root / item["asset"]
        assert path.suffix == extension and path.name == item["asset"]
        assert path.stat().st_size == item["size"] and digest(path) == item["sha256"]
        assert len(list(root.glob(f"*{extension}"))) == 1
        manifest[platform] = item
        assets.append(path)
    manifest["macos_source_commit"] = os.environ["SOURCE_SHA"]
    (root / "release-provenance.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    assets.extend(sorted(root.glob("*.zip")))
    (root / "SHA256SUMS.txt").write_text("".join(f"{digest(path)}  {path.name}\n" for path in assets), encoding="utf-8")
    for path in root.glob("*-provenance.json"):
        if path.name != "release-provenance.json":
            path.unlink()


if __name__ == "__main__":
    main()
