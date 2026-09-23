"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds the seven GLSL pieces `source/Shaders.cpp` assembles its
five passes from, plus the version line. That is two copies of the same text,
and two copies drift -- quietly, because a demo that renders a *plausible*
picture looks exactly like a demo that renders the right one. The whole claim of
these pages is that they run the plugin's own shader rather than something
reimplemented to look similar, so the claim needs something enforcing it.

Nothing else can. `rbtest` drives the real plugin class and has no idea this
page exists, and `tools/verify.sh`'s glslc step compiles the C++ copies and
never looks at the JS one.

------------------------------------------------------------------- what it does

1. Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
   out of `plugin.js`, and compares them exactly -- no whitespace normalisation,
   no comment stripping. A comment updated on one side and not the other is
   exactly the drift worth catching: the comments in this repo carry the
   reasoning (why the grain is integer hashing, why the edge print reads with
   explicit gradients, why the scan returns early at Mix 1).
   None of the GLSL contains a backtick or a backslash, so the JS side must not
   contain a backslash either; one there could only be hiding a difference.
2. The version line, which is a plain string literal on both sides.
3. The assembly: which body each pass is built from and whether it gets the
   model library. `Vertex()` .. `Scan()` in the C++, the `assemble(...)` table
   in plugin.js. A pass that silently lost `kModel` would not compile, but one
   that gained it would, and would still be wrong.
4. The 5x7 glyph table, row for row against `source/Font.cpp`. It is data, not a
   port, so it can be checked, and a changed glyph would change the edge print.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. `develop`, `scannerProfile`,
`computeGeometry`, `buildText` and every conversion in plugin.js are a hand
translation of Model.cpp, Frame.cpp, Controls.cpp and Rebate::ProcessOpenGL,
and only a reader can tell whether they still agree. When you change one of
those, change plugin.js too -- a wrong number there shows up on the page as a
film that is subtly the wrong colour, which nobody will notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ symbol, both in source/Shaders.cpp.
SHADERS = [
    ("VERTEX_BODY", "kVertexBody"),
    ("MODEL", "kModel"),
    ("COPY_BODY", "kCopyBody"),
    ("FILM_BODY", "kFilmBody"),
    ("BLOCKS_BODY", "kBlocksBody"),
    ("LEVELS_BODY", "kLevelsBody"),
    ("SCAN_BODY", "kScanBody"),
]

# The C++ function, the JS constant it becomes.
PASSES = [
    ("Vertex", "VERTEX"),
    ("Copy", "COPY"),
    ("Film", "FILM"),
    ("Blocks", "BLOCKS"),
    ("Levels", "LEVELS"),
    ("Scan", "SCAN"),
]

JS_OF_CPP = {cpp: js for js, cpp in SHADERS}


def read(path):
    with open(os.path.join(REPO, path)) as handle:
        return handle.read()


def from_cpp(source, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = body.find("\\")
    if stray >= 0:
        return None, f"backslash at line {body[:stray].count(chr(10)) + 1}"
    if "${" in body:
        return None, "${ -- a template interpolation"
    return body, None


def cpp_assembly(source, function):
    match = re.search(
        r'std::string ' + function + r'\(\)\s*\{\s*return assemble\(\s*(\w+)\s*,\s*(true|false)\s*\);\s*\}',
        source,
    )
    return None if match is None else (match.group(1), match.group(2))


def js_assembly(source, name):
    match = re.search(r'^const ' + name + r' = assemble\((\w+), (true|false)\);$', source, re.M)
    return None if match is None else (match.group(1), match.group(2))


def glyph_rows_cpp(source):
    return [
        tuple(re.findall(r'"([.#]{5})"', row))
        for row in re.findall(r'^\s*\{ ((?:"[.#]{5}",? ?){7})\}, //', source, re.M)
    ]


def glyph_rows_js(source):
    match = re.search(r'^const GLYPHS = \[\n(.*?)^\];$', source, re.S | re.M)
    if match is None:
        return None
    return [
        tuple(re.findall(r"'([.#]{5})'", row))
        for row in re.findall(r"^  \[((?:'[.#]{5}',? ?){7})\],", match.group(1), re.M)
    ]


def first_difference(a_text, b_text):
    a_lines = a_text.splitlines()
    b_lines = b_text.splitlines()
    for i in range(max(len(a_lines), len(b_lines))):
        a = a_lines[i] if i < len(a_lines) else "<missing>"
        b = b_lines[i] if i < len(b_lines) else "<missing>"
        if a != b:
            return i + 1, a, b
    return None


def main():
    cpp = read("source/Shaders.cpp")
    js = read("demo/plugin.js")
    problems = 0

    for name, symbol in SHADERS:
        cpp_text = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
        elif cpp_text == js_text:
            print(f"ok    {name:<12} matches {symbol} ({len(cpp_text)} chars)")
        else:
            problems += 1
            line, a, b = first_difference(cpp_text, js_text)
            print(f"FAIL  {name} has drifted from {symbol}")
            print(f"        first difference at line {line}")
            print(f"          C++: {a}")
            print(f"          js : {b}")

    # The version line: a plain literal on both sides, compared as source text.
    if re.search(r'^const char\* const kVersion = "#version 410 core\\n";$', cpp, re.M) is None:
        print("FAIL  kVersion is no longer \"#version 410 core\\n\" -- the kit's port() only strips that line")
        problems += 1
    elif re.search(r"^const VERSION = '#version 410 core\\n';$", js, re.M) is None:
        print("FAIL  VERSION in demo/plugin.js is not '#version 410 core\\n'")
        problems += 1
    else:
        print("ok    VERSION      matches kVersion")

    # The assembly of each pass.
    js_assemble = re.search(
        r"^function assemble\(body, withModel\) \{\n  let s = VERSION;\n  if \(withModel\) s \+= MODEL;\n  s \+= body;\n  return s;\n\}$",
        js,
        re.M,
    )
    if js_assemble is None:
        print("FAIL  assemble() in demo/plugin.js is not version + (model) + body")
        problems += 1
    for function, name in PASSES:
        c = cpp_assembly(cpp, function)
        j = js_assembly(js, name)
        if c is None:
            print(f"FAIL  {function}() in source/Shaders.cpp is not a plain assemble( body, bool )")
            problems += 1
        elif j is None:
            print(f"FAIL  {name} in demo/plugin.js is not a plain assemble(BODY, bool)")
            problems += 1
        elif (JS_OF_CPP.get(c[0]), c[1]) != j:
            print(f"FAIL  {name} is assemble{j}, but {function}() is assemble{c}")
            problems += 1
        else:
            print(f"ok    {name:<12} is assembled as {function}(): {c[0]}, model {c[1]}")

    # The glyph table.
    cpp_glyphs = glyph_rows_cpp(read("source/Font.cpp"))
    js_glyphs = glyph_rows_js(js)
    if len(cpp_glyphs) != 96:
        print(f"FAIL  found {len(cpp_glyphs)} glyphs in source/Font.cpp, expected 96")
        problems += 1
    elif js_glyphs is None or len(js_glyphs) != len(cpp_glyphs):
        print(f"FAIL  GLYPHS in demo/plugin.js has {0 if js_glyphs is None else len(js_glyphs)} rows, the C++ {len(cpp_glyphs)}")
        problems += 1
    else:
        wrong = [i + 32 for i, (a, b) in enumerate(zip(cpp_glyphs, js_glyphs)) if a != b]
        if wrong:
            print(f"FAIL  GLYPHS differs from Font.cpp at codes {wrong}")
            problems += 1
        else:
            print(f"ok    GLYPHS       matches Font.cpp's kGlyphs (96 glyphs)")

    print()
    if problems:
        print(f"{problems} problem(s) -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shader pieces, the assembly of all {len(PASSES)} passes and the glyph table are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
