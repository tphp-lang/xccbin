#!/usr/bin/env python3
# mirror_cache.py — 把 pins/*.json 锁定的源文件镜像到指定目录
#
# 用途：CI (mirror-cache.yml) 运行本脚本，把 17 个锁定文件下载到
#       cache-mirror/ 并逐个核对 pins 里的 sha256，再上传到本仓库
#       Release (tag: sysroot-cache)。此后 pins/*.json 的 url 切到
#       Release 资产，CI 采集改从 GitHub CDN 拉取，不再依赖
#       aliyun/debian 源的速度与可用性。
#
# 幂等：目标目录中已存在且 sha256 匹配的文件跳过下载，复跑零流量。
#
# 用法：python3 tools/mirror_cache.py [输出目录]   （默认 cache-mirror）
import hashlib
import json
import sys
import urllib.request
from pathlib import Path


def sha256_of(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main() -> None:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else 'cache-mirror')
    out.mkdir(parents=True, exist_ok=True)
    pin_files = sorted(Path('pins').glob('*.json'))
    if not pin_files:
        sys.exit('pins/*.json 不存在')

    total = downloaded = 0
    for pf in pin_files:
        for it in json.load(open(pf))['items']:
            dst = out / it['file']
            want = it['sha256'].lower()
            if dst.is_file() and sha256_of(dst) == want:
                print(f"  跳过(已匹配) {it['file']}")
            else:
                print(f"  下载 {it['url']}")
                req = urllib.request.Request(
                    it['url'], headers={'User-Agent': 'xcc-mirror/1.0'})
                with urllib.request.urlopen(req, timeout=300) as r, \
                        open(dst, 'wb') as f:
                    while True:
                        b = r.read(1 << 20)
                        if not b:
                            break
                        f.write(b)
                got = sha256_of(dst)
                if got != want:
                    dst.unlink()
                    sys.exit(f"sha256 不符: {it['file']}\n  期望 {want}\n  实际 {got}")
                downloaded += 1
            total += 1
    print(f"[mirror] {total} 个锁定文件就绪（本次下载 {downloaded} 个），目录: {out}")


if __name__ == '__main__':
    main()
