"""luachk — проверка Lua-чанков EVBots.dll: вырезает каждый блок R"LUA( ... )LUA" из .cpp и компилирует его lua54 (loadfile).

Часть финишной черты сборки (CLAUDE.md): «Lua-чанки компилируются (luachk)».
Запуск:  python -I tools\\luachk.py src\\*.cpp      ->  «chunks N bad 0»; при ошибке — BAD <файл> block at line <строка> <текст>.
lua54.exe — C:\\Games\\tools\\lua54 (с 23.10.2026; раньше лежал в trash\\2026-10-10\\lua54).
"""
import glob, os, re, shutil, subprocess, sys, tempfile

LUA = r"C:\Games\tools\lua54\lua54.exe"

paths = []
for a in sys.argv[1:]:
    paths += glob.glob(a) or [a]
out = tempfile.mkdtemp(prefix="luachk-")
bad = total = 0
try:
    for path in paths:
        src = open(path, encoding="utf-8").read()
        for i, m in enumerate(re.finditer(r'R"LUA\((.*?)\)LUA"', src, re.S)):
            total += 1
            f = os.path.join(out, "%s_%d.lua" % (os.path.basename(path), i))
            open(f, "w", encoding="utf-8").write(m.group(1))
            r = subprocess.run([LUA, "-e", "local f,e=loadfile([[%s]]) if not f then print(e) os.exit(1) end" % f],
                               capture_output=True, text=True)
            if r.returncode != 0:
                bad += 1
                line = src[:m.start()].count("\n") + 1
                print("BAD", path, "block at line", line, (r.stdout + r.stderr).strip()[:300])
finally:
    shutil.rmtree(out, ignore_errors=True)
print("chunks", total, "bad", bad)
sys.exit(1 if bad else 0)
