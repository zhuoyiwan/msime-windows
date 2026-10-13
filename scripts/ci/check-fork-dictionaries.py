"""改版使用固定文本源重建词库；测试与安装包都校验这份来源和释义覆盖。"""

import argparse
from contextlib import closing
import json
from pathlib import Path
import re
import sqlite3
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from dictionary_build import build, inputs
import dictionary_product


def verify(directory):
    lock = json.loads((ROOT / "fork-dictionary-lock.json").read_text(encoding="utf-8"))
    marker = json.loads((directory / "local-dictionary.json").read_text(encoding="utf-8"))
    if (marker.get("repository") != lock["repository"]
            or marker.get("custom_dictionary_commit") != lock["source_commit"]
            or marker.get("dirty") is not False or marker.get("builder") != build.builder_digest()):
        raise ValueError("词库来源、工作树状态或构建器与改版锁不一致")
    required = set(dictionary_product.DESKTOP_FILES) | set(build.NOTICE_FILES) | {dictionary_product.MANIFEST_NAME, "SHA256SUMS.txt"}
    if set(marker["files"]) != required:
        raise ValueError("词库摘要清单缺文件或包含未知产物")
    for name, digest in marker["files"].items():
        if Path(name).name != name or inputs.sha256(directory / name) != digest:
            raise ValueError(f"词库摘要不一致：{name}")
    dictionary_product.verify_product(directory, "desktop")
    with closing(sqlite3.connect((directory / "english.db").resolve().as_uri() + "?mode=ro", uri=True)) as db:
        counts = {table: db.execute(f"SELECT count(*) FROM {table}").fetchone()[0]
                  for table in ("zh_en_glosses", "en_zh_glosses")}
        for table, count in counts.items():
            if count < lock[f"minimum_{table}"]:
                raise ValueError(f"{table} 覆盖退化：{count}")
        for word, expected in {"我": "pron.", "想": "v.", "去": "v.", "吃": "v.", "苹果": "n."}.items():
            row = db.execute("SELECT english_gloss FROM zh_en_glosses WHERE chinese=?", (word,)).fetchone()
            if not row or expected not in row[0]:
                raise ValueError(f"常用词释义或词性缺失：{word}")
    return marker, counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, default=ROOT / "MetasequoiaImeDict/out")
    parser.add_argument("--product-manifest", type=Path)
    parser.add_argument("--windows-commit")
    args = parser.parse_args()
    marker, counts = verify(args.directory)
    if args.product_manifest:
        if not re.fullmatch(r"[0-9a-f]{40}", args.windows_commit or ""):
            raise ValueError("Windows 来源必须是完整提交 SHA")
        data = {"schema_version": 1, "repositories": {"windows": {
            "repository": "zhuoyiwan/msime-windows", "commit": args.windows_commit}},
            "dictionary": marker, "gloss_counts": counts}
        args.product_manifest.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(counts, ensure_ascii=False))


if __name__ == "__main__":
    main()
