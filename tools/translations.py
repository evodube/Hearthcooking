#!/usr/bin/env python3
"""Writes the translators' template, assets/translations/Hearthcooking_ENGLISH.txt: every line of the kitchen's
English, from Tr("..."), TrF("..."), N_("...") and L("...") in the plugin's code, the verdict lines of the kitchen's
rules (src/sim/Kitchen.cpp) and the sound names (Config.h). --check fails if the file in the repo is out of date."""
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
out = root / "assets" / "translations" / "Hearthcooking_ENGLISH.txt"
lit = r'"((?:[^"\\]|\\.)*)"'
marked = re.compile(r'\b(?:Tr|TrF|N_|L)\(\s*' + lit)

def unescape(s):
    return bytes(s, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")

lines = []
def add(s):
    s = unescape(s)
    if s and (re.search(r"[A-Za-z]", s) or "{" in s) and s not in lines:
        lines.append(s)

for f in sorted((root / "src" / "plugin").glob("*.cpp")):
    for m in marked.finditer(f.read_text(encoding="utf-8")):
        add(m.group(1))
for l in (root / "src" / "sim" / "Kitchen.cpp").read_text(encoding="utf-8").splitlines():
    if l.lstrip().startswith("#"):
        continue
    for m in re.finditer(lit, l.split("//")[0]):
        add(m.group(1))
cfg = (root / "src" / "plugin" / "Config.h").read_text(encoding="utf-8")
block = re.search(r"kSoundLabels\[\]\s*=\s*\{(.*?)\};", cfg, re.S)
for m in re.finditer(lit, block.group(1)):
    add(m.group(1))

head = """// Hearthcooking's words. To translate: copy this file to Hearthcooking_<LANGUAGE>.txt in the same folder (the game's
// language as Skyrim.ini's sLanguage has it: KOREAN, FRENCH, GERMAN, RUSSIAN, CHINESE, JAPANESE, POLISH, SPANISH,
// ITALIAN...) and write each line's translation after its TAB, in place of the English. Save as UTF-16 LE (as the
// game's own translation files) or UTF-8.
// - The English before the TAB must stay exactly as it is: it's how the kitchen finds the line.
// - {} is a gap the kitchen fills in (an ingredient's name, a number). Keep as many as the English has. To put them in
//   another order, number them: {0} is the first, {1} the second ("{} {}" -> "{1} ({0})").
// - A line you leave out, or leave English, stays English. Lines starting with // are notes.
// - Item and recipe names are the game's own (from your game's translation), not in here.
// - In game: Mod Configuration (SKSE Menu Framework) > Hearthcooking > Language > Reload the translation shows changes
//   without a restart; Hearthcooking.log lists every line it showed that isn't translated yet.
"""
body = head + "".join(f"{l.replace(chr(92), chr(92) * 2).replace(chr(10), chr(92) + 'n')}\t{l.replace(chr(92), chr(92) * 2).replace(chr(10), chr(92) + 'n')}\n" for l in lines)
data = b"\xff\xfe" + body.replace("\n", "\r\n").encode("utf-16-le")
if "--check" in sys.argv:
    if not out.exists() or out.read_bytes() != data:
        print("assets/translations/Hearthcooking_ENGLISH.txt is out of date: run python tools/translations.py")
        sys.exit(1)
    print(f"translation template up to date ({len(lines)} lines)")
else:
    out.write_bytes(data)
    print(f"wrote {out.relative_to(root)}: {len(lines)} lines")
