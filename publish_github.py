#!/usr/bin/env python3
"""把本目录的内容发布到你的 GitHub（走 REST API，不依赖 git 命令行）。

用法：
    set GITHUB_TOKEN=ghp_xxx          # PowerShell: $env:GITHUB_TOKEN="ghp_xxx"
    python publish_github.py <repo名> [--private] [--desc "描述"]

例：
    python publish_github.py stellaris-largepage
    python publish_github.py stellaris-largepage --private

要求 token 权限：
  * 经典 token：勾选 repo（创建仓库 + 写文件）
  * 细粒度 token：Administration=Read/Write（建仓）+ Contents=Read/Write（写文件）
仅当你要新建仓库时才需要 Administration；往已有仓库推只需要 Contents。

脚本只读取本目录下的白名单文件，绝不会上传 backup/ 里的游戏本体。
"""

import base64
import json
import os
import sys
import urllib.error
import urllib.request

API = "https://api.github.com"
HERE = os.path.dirname(os.path.abspath(__file__))
UPLOAD = ["README.md", "LICENSE", ".gitignore", "patch_stellaris.py",
          "lpshim.c", "lpshim.cfg", "lpshim.dll"]


def api(path, method="GET", payload=None, token=None):
    req = urllib.request.Request(API + path, method=method)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "stellaris-largepage-publisher")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    data = None
    if payload is not None:
        data = json.dumps(payload).encode()
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, data, timeout=60) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        return e.code, {"error": body}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if not args:
        print(__doc__)
        return 1
    repo = args[0]
    private = "--private" in flags
    desc = "Stellaris 4.5.x large-page (2MB) allocator shim - static patch, no injector"
    if "--desc" in flags:
        desc = sys.argv[sys.argv.index("--desc") + 1]

    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not token:
        print("!! 没有 token：请先设置环境变量 GITHUB_TOKEN")
        return 1

    st, me = api("/user", token=token)
    if st != 200:
        print("!! token 无效：", me)
        return 1
    login = me["login"]
    print(f"已认证为 {login}")

    st, info = api(f"/repos/{login}/{repo}", token=token)
    if st == 200:
        print(f"仓库已存在：{info['html_url']}")
    else:
        st, info = api("/user/repos", method="POST", token=token,
                       payload={"name": repo, "private": private, "description": desc,
                                "has_issues": True, "has_wiki": False, "auto_init": False})
        if st not in (200, 201):
            print("!! 建仓失败：", info)
            return 1
        print(f"仓库已创建：{info['html_url']}  (private={private})")

    for name in UPLOAD:
        path = os.path.join(HERE, name)
        if not os.path.exists(path):
            print(f"  - 跳过（不存在）{name}")
            continue
        content = base64.b64encode(open(path, "rb").read()).decode()
        st, cur = api(f"/repos/{login}/{repo}/contents/{name}", token=token)
        payload = {"message": f"add {name}", "content": content}
        if st == 200:
            payload["sha"] = cur["sha"]
            payload["message"] = f"update {name}"
        st, res = api(f"/repos/{login}/{repo}/contents/{name}", method="PUT",
                      payload=payload, token=token)
        print(f"  {'✓' if st in (200, 201) else '✗'} {name}  ({st})")
        if st not in (200, 201):
            print("     ", str(res)[:300])

    print(f"\n完成 → https://github.com/{login}/{repo}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
