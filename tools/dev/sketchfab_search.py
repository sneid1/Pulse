# Search the Sketchfab v3 API for downloadable models and print a compact candidate
# table (name / uid / license / polycount / url). Token read from tools/sketchfab.key
# (gitignored). Usage:
#   python tools/dev/sketchfab_search.py "modular sci-fi wall" [more queries...]
import json
import os
import sys
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOKEN = open(os.path.join(ROOT, "tools", "sketchfab.key")).read().strip()

# Permissive licenses OK for this project (CC-BY-4.0 with attribution already in use).
OK_LICENSES = {"by", "cc-by", "cc0", "cc-by-40", "cc-by"}


def api(url):
    req = urllib.request.Request(url, headers={"Authorization": "Token " + TOKEN})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def search(q):
    params = {
        "type": "models",
        "q": q,
        "downloadable": "true",
        "count": "24",
        "sort_by": "-likeCount",
    }
    url = "https://api.sketchfab.com/v3/search?" + urllib.parse.urlencode(params)
    return api(url).get("results", [])


def lic(m):
    li = m.get("license") or {}
    return (li.get("slug") or li.get("label") or "?")


def main():
    queries = sys.argv[1:] or ["modular sci-fi wall"]
    seen = set()
    rows = []
    for q in queries:
        for m in search(q):
            uid = m["uid"]
            if uid in seen:
                continue
            seen.add(uid)
            rows.append((q, m))
    # permissive first, then by face count ascending
    def keyf(item):
        _, m = item
        l = lic(m).lower()
        perm = 0 if l in OK_LICENSES else 1
        return (perm, m.get("faceCount") or 1 << 30)
    rows.sort(key=keyf)
    print("LIC    FACES    VERTS    UID                               NAME  [query]")
    for q, m in rows:
        print("%-6s %-8s %-8s %s  %s  [%s]  by %s" % (
            lic(m),
            m.get("faceCount", "?"),
            m.get("vertexCount", "?"),
            m["uid"],
            (m.get("name") or "")[:50],
            q,
            (m.get("user") or {}).get("username", "?"),
        ))


if __name__ == "__main__":
    main()
