# -*- coding: utf-8 -*-
"""Legacy helper: маленькие операции с файлами без мучений с powershell/cmd.
Примеры:
  python tools.py head C:\\Games\\Legacy\\LegacyBF3\\VU-API-SERVER.md 20
  python tools.py headers C:\\Games\\Legacy\\LegacyBF3\\VU-API-SERVER.md
  python tools.py count C:\\Games\\Legacy\\LegacyBF3\\VU-API-SERVER.md
"""
import sys, io
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

def head(path, n=30):
    with io.open(path, encoding="utf-8") as f:
        for i, line in enumerate(f):
            if i >= n: break
            sys.stdout.write(line.rstrip("\n") + "\n")

def headers(path):
    with io.open(path, encoding="utf-8") as f:
        for line in f:
            s = line.rstrip("\n")
            if s.startswith("#"):
                print(s)

def count(path):
    with io.open(path, encoding="utf-8") as f:
        s = f.read()
    print("chars", len(s), "lines", s.count("\n") + 1)

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "help"
    if cmd == "head" and len(sys.argv) >= 3:
        head(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 30)
    elif cmd == "headers" and len(sys.argv) >= 3:
        headers(sys.argv[2])
    elif cmd == "count" and len(sys.argv) >= 3:
        count(sys.argv[2])
    else:
        print(__doc__)
