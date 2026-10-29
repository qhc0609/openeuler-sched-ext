#!/usr/bin/env python3
"""Push the local HEAD commit to GitHub through the Git Data API.

Why this exists: on this machine `github.com:443` is intermittently
unreachable while `api.github.com` stays up, so `git push` dies with
"Empty reply from server".  The Git Data API lives on api.github.com and
can rebuild a commit from blobs/trees, which is all a push really is.

The commit is rebuilt with the *exact* tree, parents, author, committer and
message of the local commit, so the resulting object hash is bit-identical
to the local one.  If the hash does not match, something was altered and
the script refuses to move the ref.
"""

import base64
import datetime
import json
import os
import subprocess
import sys

REPO = "qhc0609/openeuler-sched-ext"
BRANCH = "main"
GH = r"C:\Program Files\GitHub CLI\gh.exe"
GIT = r"C:\Program Files\Git\cmd\git.exe"

REPO_DIR = os.path.dirname(os.path.abspath(__file__))


def git(*args):
    # core.quotepath=false：文件名里有中文时，git 默认会把路径转义成
    # "articles/\344\270\272..."，这种字符串再喂回 rev-parse 就找不到对象。
    # 关掉转义，路径才是原样的 UTF-8。
    out = subprocess.run(
        [GIT, "-c", "core.quotepath=false", *args],
        cwd=REPO_DIR, capture_output=True, check=True,
    )
    return out.stdout


def gh_api(method, endpoint, payload=None, jq=None):
    cmd = [GH, "api", "--method", method, endpoint]
    if jq:
        cmd += ["--jq", jq]
    stdin = None
    if payload is not None:
        stdin = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        cmd += ["--input", "-"]
    r = subprocess.run(cmd, input=stdin, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(
            "gh api %s %s failed:\n%s\n"
            % (method, endpoint, r.stderr.decode("utf-8", "replace"))
        )
        sys.exit(1)
    text = r.stdout.decode("utf-8", "replace").strip()
    return text if jq else json.loads(text or "{}")


def parse_commit(raw):
    """Split a raw commit object into header lines and message body."""
    head, _, message = raw.partition(b"\n\n")
    lines = head.decode("utf-8").split("\n")
    tree = parent = author = committer = None
    for line in lines:
        if line.startswith("tree "):
            tree = line[5:]
        elif line.startswith("parent "):
            parent = line[7:]
        elif line.startswith("author "):
            author = line[7:]
        elif line.startswith("committer "):
            committer = line[10:]
    return tree, parent, author, committer, message.decode("utf-8")


def split_ident(ident):
    """'Name <email> 1792921135 +0800' -> (name, email, iso8601 date).

    git stores the date as '<epoch> <+-HHMM>'; the API only accepts ISO 8601.
    Both encode the same instant, and GitHub normalises it back to the git
    form internally, so the commit hash is unaffected.
    """
    name, _, rest = ident.partition(" <")
    email, _, date = rest.partition("> ")
    epoch, _, tz = date.partition(" ")
    sign = 1 if tz[0] == "+" else -1
    offset = sign * (int(tz[1:3]) * 60 + int(tz[3:5]))
    local = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc)
    local = local.astimezone(datetime.timezone(datetime.timedelta(minutes=offset)))
    return name, email, local.isoformat()


def main():
    # 默认推 HEAD；也可以显式给一个提交，用来按顺序推一串还没上去的提交。
    # 必须一个一个来：base_tree 要在远端存在，否则 GitHub 报
    # "base_tree is not a valid tree oid"。
    target = sys.argv[1] if len(sys.argv) > 1 else "HEAD"

    raw = git("cat-file", "commit", target)
    tree, parent, author, committer, message = parse_commit(raw)
    local_sha = git("rev-parse", target).decode().strip()

    print("local commit : %s" % local_sha)
    print("  tree       : %s" % tree)
    print("  parent     : %s" % parent)
    print("  author     : %s" % author)
    print("  committer  : %s" % committer)

    # --- 1. which paths differ from the parent -------------------------
    # -z：用 \0 分隔，路径里有空格也不会被拆开
    diff = [
        p.decode("utf-8")
        for p in git("diff", "--name-only", "-z", "%s..%s" % (parent, target)).split(b"\0")
        if p
    ]
    if not diff:
        print("nothing to push: %s == parent" % target)
        return

    # --- 2. upload a blob per changed file, verify hash ----------------
    entries = []
    for path in diff:
        # 这一版提交里被删掉（或改名走了）的文件：GitHub 的 tree API 用
        # "sha": null 表示删除，不能当成空 blob 传上去，否则远端会留一个
        # 0 字节的文件，本地又看不到它。
        try:
            blob_sha = git("rev-parse", "%s:%s" % (target, path)).decode().strip()
        except subprocess.CalledProcessError:
            print("\ndelete %-24s" % path)
            entries.append({"path": path, "mode": "100644",
                            "type": "blob", "sha": None})
            continue
        mode = git("ls-tree", target, "--", path).decode().split()[0]
        content = git("cat-file", "blob", "%s:%s" % (target, path))
        print("\nblob %-24s %8d bytes  %s" % (path, len(content), blob_sha[:12]))

        got = gh_api(
            "POST",
            "repos/%s/git/blobs" % REPO,
            {
                "content": base64.b64encode(content).decode("ascii"),
                "encoding": "base64",
            },
            jq=".sha",
        )
        if got != blob_sha:
            sys.stderr.write(
                "blob hash mismatch for %s: local %s remote %s\n"
                % (path, blob_sha, got)
            )
            sys.exit(1)
        print("  uploaded, hash matches")

        entries.append(
            {
                "path": path,
                "mode": "100755" if mode == "100755" else "100644",
                "type": "blob",
                "sha": blob_sha,
            }
        )

    # --- 3. tree ------------------------------------------------------
    # base_tree = parent's tree, so unchanged files are inherited and the
    # resulting tree hash must equal the local one.
    parent_tree = git("rev-parse", "%s^{tree}" % parent).decode().strip()
    new_tree = gh_api(
        "POST",
        "repos/%s/git/trees" % REPO,
        {"base_tree": parent_tree, "tree": entries},
        jq=".sha",
    )
    print("\ntree\n  local  %s\n  remote %s" % (tree, new_tree))
    if new_tree != tree:
        sys.stderr.write("tree hash mismatch - refusing to continue\n")
        sys.exit(1)
    print("  match")

    # --- 4. commit ----------------------------------------------------
    a_name, a_email, a_date = split_ident(author)
    c_name, c_email, c_date = split_ident(committer)
    new_commit = gh_api(
        "POST",
        "repos/%s/git/commits" % REPO,
        {
            "message": message,
            "tree": new_tree,
            "parents": [parent],
            "author": {"name": a_name, "email": a_email, "date": a_date},
            "committer": {"name": c_name, "email": c_email, "date": c_date},
        },
        jq=".sha",
    )
    print("\ncommit\n  local  %s\n  remote %s" % (local_sha, new_commit))
    if new_commit != local_sha:
        sys.stderr.write("commit hash mismatch - refusing to move the ref\n")
        sys.exit(1)
    print("  match - byte-identical commit object")

    # --- 5. move the branch -------------------------------------------
    gh_api(
        "PATCH",
        "repos/%s/git/refs/heads/%s" % (REPO, BRANCH),
        {"sha": new_commit, "force": False},
    )
    head = gh_api(
        "GET", "repos/%s/git/refs/heads/%s" % (REPO, BRANCH), jq=".object.sha"
    )
    print("\nremote %s -> %s" % (BRANCH, head))
    print("RESULT: %s" % ("PUSHED, hashes identical" if head == local_sha else "FAILED"))


if __name__ == "__main__":
    main()
