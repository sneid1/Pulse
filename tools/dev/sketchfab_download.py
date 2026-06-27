# Download a Sketchfab model's glTF archive and extract it under
# assets/external/sketchfab_scifi/<dest>/. Token read from tools/sketchfab.key.
# Usage: python tools/dev/sketchfab_download.py <uid> <dest_dirname>
import io
import json
import os
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOKEN = open(os.path.join(ROOT, "tools", "sketchfab.key")).read().strip()


def api(url):
    req = urllib.request.Request(url, headers={"Authorization": "Token " + TOKEN})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def main():
    uid, dest = sys.argv[1], sys.argv[2]
    dl = api("https://api.sketchfab.com/v3/models/" + uid + "/download")
    gltf = dl.get("gltf")
    if not gltf or not gltf.get("url"):
        print("NO_GLTF_ARCHIVE", json.dumps({k: list(v) if isinstance(v, dict) else v for k, v in dl.items()}))
        sys.exit(1)
    url = gltf["url"]
    print("Downloading gltf archive (%s bytes)..." % gltf.get("size"))
    with urllib.request.urlopen(url, timeout=180) as r:
        data = r.read()
    outdir = os.path.join(ROOT, "assets", "external", "sketchfab_scifi", dest)
    os.makedirs(outdir, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        z.extractall(outdir)
        names = z.namelist()
    print("EXTRACTED %d files to %s" % (len(names), outdir))
    for n in names[:60]:
        print("  ", n)


if __name__ == "__main__":
    main()
