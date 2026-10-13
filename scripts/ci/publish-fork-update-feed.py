"""公开安装包核验后发布静态更新清单；独立分支只存清单，不携带客户端凭据。"""

import argparse
import base64
import json
from pathlib import Path
import re
import subprocess

REPOSITORY = "zhuoyiwan/msime-windows"
BRANCH = "zhuoyiwan-updates"
PREFIX = f"https://github.com/{REPOSITORY}/releases/download/"
ASSET = re.compile(r"(?:MetasequoiaIME_Setup_v\d+\.\d+\.\d+-zhuoyiwan-local-\d{8}-r\d+(-unsigned)?\.exe|"
                   r"MetasequoiaIME_Android_v\d+\.\d+\.\d+-zhuoyiwan-arm64-r\d+-signed\.apk)\Z")


def release_entry(release):
    tag = release.get("tag_name", "")
    if release.get("draft") is not False or not re.fullmatch(r"zhuoyiwan-[A-Za-z0-9._-]{1,100}", tag):
        raise ValueError("更新清单只接受已公开的改版发布")
    assets = []
    for item in release["assets"]:
        name = item["name"]
        if not ASSET.fullmatch(name):
            continue
        if (not re.fullmatch(r"sha256:[0-9a-f]{64}", item.get("digest") or "")
                or item["browser_download_url"] != PREFIX + tag + "/" + name
                or type(item["size"]) is not int or not 0 < item["size"] <= 512 * 1024 * 1024):
            raise ValueError(f"公开资产来源、摘要或大小不合法：{name}")
        assets.append({key: item[key] for key in ("name", "size", "digest", "browser_download_url")})
    if len(assets) != 2 or sorted(Path(item["name"]).suffix for item in assets) != [".apk", ".exe"]:
        raise ValueError("缺少唯一 Windows/Android 安装包")
    return {"tag_name": tag, "draft": False, "prerelease": bool(release["prerelease"]), "assets": assets}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--release-id", required=True, type=int)
    parser.add_argument("--gh-wrapper", type=Path)
    args = parser.parse_args()
    command = ["pwsh", "-NoProfile", "-File", str(args.gh_wrapper)] if args.gh_wrapper else ["gh"]

    def api(path, data=None, missing=False, method=None):
        cmd = command + ["api", f"repos/{REPOSITORY}/{path}"]
        if data is not None:
            cmd += ["--method", method or "POST", "--input", "-"]
        result = subprocess.run(cmd, input=json.dumps(data) if data is not None else None,
                                capture_output=True, text=True, encoding="utf-8")
        if result.returncode:
            if missing and "404" in result.stderr:
                return None
            raise RuntimeError(f"GitHub 更新清单操作失败：{path}: {result.stderr}")
        return json.loads(result.stdout)

    entry = release_entry(api(f"releases/{args.release_id}"))
    head = api(f"git/ref/heads/{BRANCH}", missing=True)
    previous = []
    if head:
        content = api(f"contents/releases.json?ref={BRANCH}")
        previous = json.loads(base64.b64decode(content["content"]))
    entries = [entry] + [release_entry(item) for item in previous if item["tag_name"] != entry["tag_name"]]
    payload = json.dumps(entries[:30], ensure_ascii=False, indent=2) + "\n"
    blob = api("git/blobs", {"content": payload, "encoding": "utf-8"})
    tree = api("git/trees", {"tree": [{"path": "releases.json", "mode": "100644", "type": "blob", "sha": blob["sha"]}]})
    commit = api("git/commits", {"message": f"chore(installer): 更新 {entry['tag_name']} 静态更新清单",
                                "tree": tree["sha"], "parents": [head["object"]["sha"]] if head else []})
    if head:
        api(f"git/refs/heads/{BRANCH}", {"sha": commit["sha"], "force": False}, method="PATCH")
    else:
        api("git/refs", {"ref": f"refs/heads/{BRANCH}", "sha": commit["sha"]})
    print(f"更新清单已发布：{entry['tag_name']}")


if __name__ == "__main__":
    main()
