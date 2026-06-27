# Print Sketchfab model metadata + available download archives for given UIDs.
# Usage: python tools/dev/sketchfab_info.py <uid> [uid...]
import json
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOKEN = open(os.path.join(ROOT, "tools", "sketchfab.key")).read().strip()


def api(url):
    req = urllib.request.Request(url, headers={"Authorization": "Token " + TOKEN})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)
    except Exception as e:  # noqa: BLE001
        return {"_error": str(e)}


for uid in sys.argv[1:]:
    m = api("https://api.sketchfab.com/v3/models/" + uid)
    if "_error" in m:
        print("== %s ERROR %s" % (uid, m["_error"]))
        continue
    li = m.get("license") or {}
    desc = (m.get("description") or "").replace("\n", " ").strip()
    tags = ",".join(t.get("slug", "") for t in (m.get("tags") or []))[:160]
    print("=" * 90)
    print("NAME: %s   by %s" % (m.get("name"), (m.get("user") or {}).get("username")))
    print("UID : %s" % uid)
    print("LIC : %s (%s)" % (li.get("label"), li.get("slug")))
    print("POLY: faces=%s verts=%s" % (m.get("faceCount"), m.get("vertexCount")))
    print("TAGS: %s" % tags)
    print("DESC: %s" % desc[:400])
    dl = api("https://api.sketchfab.com/v3/models/" + uid + "/download")
    if "_error" in dl:
        print("DOWNLOAD: ERROR %s" % dl["_error"])
    else:
        for k, v in dl.items():
            if isinstance(v, dict) and "size" in v:
                print("  archive[%s]: size=%s bytes  expires=%ss" % (k, v.get("size"), v.get("expires")))
