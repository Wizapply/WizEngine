#!/usr/bin/env python3
# 手元で .glb を作る（Meshy の代わりの、ローカルの Text to 3D の第 1 段）。
#
#   依頼文 ──► LLM（OpenAI 互換。接続先は config.json の ai.llm と同じ）
#            └─ cad.py の API で build(m) を書いた Python スクリプト
#   スクリプト ──► 別プロセスで実行（制限付き・時間切れあり）──► .glb（+ 確認用 .png）
#            └─ 例外・浮いた部品があれば、その内容を LLM に返して直させる
#
# 使い方は README.md。スクリプトは .glb の隣に残るので、手で直して
# `--script` で作り直せる（samples/v6_engine と同じ「コードが正」の流儀）。
import argparse
import ast
import json
import os
import re
import ssl
import subprocess
import sys
import tempfile
import textwrap
import time
import traceback
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
DEFAULT_OUT = os.path.join(REPO, "assets", "generated", "local")

# ---------------------------------------------------------------------------
# config.json（コメント付き JSON）を読む
# ---------------------------------------------------------------------------


def strip_json_comments(text):
    """// と /* */ を取り除く（文字列の中は触らない）。エンジンの AppConfig と同じ書式。"""
    out, i, n = [], 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
        elif c == '"':
            in_str = True
            out.append(c)
            i += 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out).lstrip("﻿")


def load_llm_config(path=None):
    """ai.llm を返す。--config が無ければ build/config.json → config.json の順に探す。"""
    cands = [path] if path else [os.path.join(REPO, "build", "config.json"), os.path.join(REPO, "config.json")]
    cfg, used = {}, None
    for p in cands:
        if p and os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                cfg = json.loads(strip_json_comments(f.read()))
            used = p
            break
    if path and not used:
        sys.exit(f"config not found: {path}")
    ai = cfg.get("ai", {}) if isinstance(cfg, dict) else {}
    llm = dict(ai.get("llm", {}) or {})
    llm["caFile"] = ai.get("caFile", "")
    llm["_from"] = used
    return llm


# ---------------------------------------------------------------------------
# LLM（OpenAI 互換 Chat Completions。依存を増やさないよう urllib で）
# ---------------------------------------------------------------------------


def chat(llm, messages):
    url = llm["url"].rstrip("/") + "/chat/completions"
    body = {"model": llm["model"], "messages": messages}
    if llm.get("temperature") is not None:
        body["temperature"] = llm["temperature"]
    req = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"), method="POST")
    req.add_header("Content-Type", "application/json")
    if llm.get("apiKey"):
        req.add_header("Authorization", "Bearer " + llm["apiKey"])
    ctx = ssl.create_default_context(cafile=llm["caFile"]) if llm.get("caFile") else None
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=float(llm.get("timeoutSec") or 300), context=ctx) as r:
            data = json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:500]
        raise RuntimeError(f"LLM HTTP {e.code}: {detail}") from None
    except urllib.error.URLError as e:
        raise RuntimeError(f"cannot reach the LLM at {url}: {e.reason}") from None
    try:
        text = data["choices"][0]["message"]["content"] or ""
    except (KeyError, IndexError, TypeError):
        raise RuntimeError(f"unexpected LLM reply: {str(data)[:300]}") from None
    usage = data.get("usage") or {}
    print(f"  LLM: {len(text)} chars in {time.time() - t0:.1f} s "
          f"(tokens in {usage.get('prompt_tokens', '?')} / out {usage.get('completion_tokens', '?')})")
    return text


def extract_code(reply):
    """応答から build(m) を含むコードを取り出す。``` が複数あれば最後のものを採る。
    推論系モデルの <think> の中の下書きは捨てる。"""
    reply = re.sub(r"<think>.*?</think>", "", reply, flags=re.S)
    blocks = re.findall(r"```(?:python|py)?\s*\n(.*?)```", reply, flags=re.S)
    for b in reversed(blocks):
        if "def build" in b:
            return b.strip() + "\n"
    if "def build" in reply:
        return reply[reply.index("def build"):].strip() + "\n"
    return None


# ---------------------------------------------------------------------------
# LLM への指示文
# ---------------------------------------------------------------------------

def _read_example(name):
    path = os.path.join(HERE, "examples", name)
    return open(path, encoding="utf-8").read() if os.path.isfile(path) else ""


EXAMPLE = _read_example("chair.py")
EXAMPLE_PARTS = _read_example("cabinet.py")

SYSTEM_PROMPT = """You are a procedural 3D modeler. You write a short Python script that builds ONE object
with constructive solid geometry. The script is run by the Charon engine's local model generator and
exported as glTF (.glb) models for a physics scene (Chrono rigid bodies).

COORDINATES
- Units are METERS, Y is UP, angles are DEGREES. Model the object at its real-world size, standing on y = 0
  (its lowest point at y = 0). Its front faces +Z. Center it on x = 0, z = 0.
- Every primitive is placed by its CENTER with at=(x, y, z) (a box of height h resting on the floor has y = h/2).

API (these names are already defined; do NOT import anything)
  box(w, h, d, at=None)                      w = size in X, h = size in Y, d = size in Z
  rounded_box(w, h, d, r, at=None)           box with rounded edges of radius r
  cylinder(r, h, at=None, axis="y")          radius r, length h along axis "x" | "y" | "z"
  cone(r1, r2, h, at=None, axis="y")         r1 at the negative end, r2 at the positive end (r2 = 0: pointed)
  sphere(r, at=None)
  ellipsoid(rx, ry, rz, at=None)
  capsule(r, length, at=None, axis="y")      rod with round ends; length = distance between end centers
  torus(R, r, at=None, axis="y")             ring radius R, tube radius r, axis = ring axis
  extrude(points, depth, at=None, axis="z")  2D polygon extruded by depth (centered on at).
                                             axis "z": points are (x, y); "x": (z, y); "y": (x, z)
  revolve(profile, at=None)                  [(radius, y), ...] polyline revolved around the Y axis
                                             (vases, bottles, lamp shades, wheels lying flat)
  tube(points, r, closed=False)              pipe of radius r through 3D points (handles, wires, frames)
  hull(a, b, ...)                            convex hull (hull of spheres = smooth organic blobs)
  union(a, b, ...) / a + b                   union;  a - b  difference;  a & b  intersection
  shape.move(x, y, z)  shape.rotate(x, y, z)  shape.rotate_about((px, py, pz), x, y, z)
  shape.scale(s) / scale(x, y, z)  shape.mirror("x")  shape.symmetric("x")  (= shape + its mirror)
  material(name_or_color, metal=None, rough=None, emissive=0.0, density=None)
      names: wood dark_wood light_wood steel chrome aluminum iron gold brass copper plastic_white
             plastic_black plastic_red plastic_blue plastic_green plastic_yellow rubber fabric leather
             ceramic glass stone concrete brick paper skin leaf bark soil water
      or a color "#rrggbb" (sRGB, as it looks) with metal 0..1, rough 0..1, density kg/m^3
  m.add(shape, material)                     adds a shape to a static object (m is the build() argument)
  p = m.part(name, fixed=False, mass=None, collision="hull")    a rigid moving part (see ASSEMBLIES)
  p.add(shape, material)                     adds a shape to that part
  m.joint(child, parent, type, anchor=(x, y, z), axis=(x, y, z), range=None, motor=None, target=0,
          stiffness=0, damping=0)            connects two parts (see ASSEMBLIES)
  math: pi sin cos tan atan2 sqrt radians degrees hypot; builtins: range len min max abs round
        int float list tuple dict enumerate zip sum sorted reversed

STATIC OBJECTS (nothing moves: furniture without doors, props, statues)
- Use only m.add(...). The whole object becomes one .glb.

ASSEMBLIES (objects with moving parts: doors, lids, drawers, wheels, rotors, arms, levers)
- Make one part per rigid moving unit with m.part("name") and add its shapes with part.add(...).
  Shapes that never move relative to each other belong to the SAME part.
- Model every part in the SAME world coordinates, in its assembled (closed / rest) pose.
- The base is usually fixed=True (it stands still). Use collision="trimesh" only for a fixed, hollow base
  (cabinet carcass, shelf, box) so things can be put inside it.
- Every other part has exactly ONE joint to its parent part (a tree): m.joint("door", "base", "hinge", ...).
  parent "world" fixes the joint to the ground.
    hinge  rotates around axis through anchor (doors, lids, wheels, rotors). Positive angles follow the
           right-hand rule around axis (thumb along axis), measured from the modeled pose.
    slide  moves along axis (drawers, sliders), measured in meters from the modeled pose.
    ball   free rotation around anchor.   weld  glued (rarely needed: use one part instead).
- anchor is where the two parts really connect (the hinge pin, the rail, the axle center); it must lie on
  or very near both parts.
- range=(lo, hi) limits a hinge (degrees) or slide (meters), e.g. a door range=(0, 110), a drawer (0, 0.35).
  Choose the axis direction so that opening is POSITIVE. Free spinning wheels: no range.
- motor="speed", target=deg/s (hinge) or m/s (slide) drives a part (fans, windmills); motor="position"
  holds a target angle / position. A joint has either range or motor, never both.
- Parts of one assembly do not collide with each other, so a drawer may sit inside the carcass and a door
  may touch the frame. Shapes WITHIN one part must still overlap.
- Masses come from the volume and the material density (override with m.part(..., mass=kg)).

RULES
- Define exactly one function: def build(m): ...
- All shapes of one part (or of a static object) form ONE connected solid: neighbouring shapes OVERLAP by
  2-5 mm (not just touch). Floating shapes and joint mistakes are reported back to you.
- Use 3 to 60 shapes and at most 12 parts. Use loops and symmetric() for repeated shapes.
- Use realistic proportions and wall thicknesses (a chair seat is ~4 cm thick, a cup wall ~4 mm).
- Hollow shapes: subtract an inner shape (a cup = cylinder - slightly smaller cylinder moved up).
- No textures, no transparency: one material per shape. 2-5 materials are usually enough.
- No import, no file access, no print, no while / try / lambda / class, no names starting with "_".

OUTPUT
- Reply with ONE ```python code block containing the whole script and nothing else.

EXAMPLE (static object)
```python
{example}```

EXAMPLE (assembly)
```python
{example_parts}```
"""


def system_prompt():
    return SYSTEM_PROMPT.replace("{example}", EXAMPLE).replace("{example_parts}", EXAMPLE_PARTS)


def user_prompt(request, height, parts=False):
    s = "Build this object:\n" + request.strip() + "\n"
    if height:
        s += f"\nIts overall height should be about {height:g} m.\n"
    if parts:
        s += ("\nBuild it as an ASSEMBLY: split it into its moving parts (m.part) and connect them with "
              "joints (m.joint) so that they move in the physics engine.\n")
    return s


# ---------------------------------------------------------------------------
# スクリプトの実行（制限付き）
# ---------------------------------------------------------------------------
# LLM が書いたコードを動かすので、構文木で危ない書き方を先に弾き、組み込み関数も
# 絞った名前空間で動かす。さらに別プロセス（時間切れ・メモリ上限つき）で回す。
# 完全な隔離ではない（Python のサンドボックスは原理的に破れる）ので、信用できない
# 接続先の LLM で回すときはコンテナや VM の中で使うこと。

FORBIDDEN_NAMES = {
    "eval", "exec", "compile", "open", "input", "getattr", "setattr", "delattr", "globals", "locals",
    "vars", "dir", "type", "object", "super", "breakpoint", "help", "exit", "quit", "memoryview",
    "classmethod", "staticmethod", "property", "__import__", "print",
}

SAFE_BUILTINS = {
    k: __builtins__[k] if isinstance(__builtins__, dict) else getattr(__builtins__, k)
    for k in ("range", "len", "min", "max", "abs", "round", "int", "float", "list", "tuple", "dict",
              "enumerate", "zip", "sum", "sorted", "reversed", "bool", "str", "isinstance",
              "ValueError", "True", "False", "None", "any", "all", "map", "filter", "set")
}


def check_source(src):
    """制限に触れる書き方の一覧（空 = 可）。行番号付きの英語（LLM に返すため）。"""
    try:
        tree = ast.parse(src)
    except SyntaxError as e:
        return [f"line {e.lineno}: syntax error: {e.msg}"]
    problems = []
    for node in ast.walk(tree):
        ln = getattr(node, "lineno", "?")
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            problems.append(f"line {ln}: import is not allowed (the API is already defined)")
        elif isinstance(node, (ast.Global, ast.Nonlocal, ast.While, ast.ClassDef, ast.AsyncFunctionDef,
                               ast.With, ast.AsyncWith, ast.Try, ast.Lambda)):
            problems.append(f"line {ln}: {type(node).__name__} is not allowed")
        elif isinstance(node, ast.Name) and (node.id.startswith("_") or node.id in FORBIDDEN_NAMES):
            problems.append(f"line {ln}: the name {node.id!r} is not allowed")
        elif isinstance(node, ast.Attribute) and node.attr.startswith("_"):
            problems.append(f"line {ln}: the attribute {node.attr!r} is not allowed")
    if not any(isinstance(n, ast.FunctionDef) and n.name == "build" for n in tree.body):
        problems.append("the script must define a top-level function build(m)")
    return problems


def run_script_inprocess(path, out_glb, height, origin, png, layer):
    """--script の本体（この関数は子プロセスで動く）。結果は dict。

    部品が 1 つ（m.add だけ）なら out_glb に 1 つの .glb。部品と関節があれば
    組み立て: <out_glb の拡張子なし>/<部品>.glb と <同>.xml（シーン文書）。"""
    sys.path.insert(0, HERE)
    import cad  # noqa: E402  （子プロセスでだけ読む = 親は manifold3d 無しでも動く）

    with open(path, encoding="utf-8") as f:
        src = f.read()
    problems = check_source(src)
    if problems:
        return {"ok": False, "error": "the script breaks the rules:\n" + "\n".join("- " + p for p in problems)}
    env = {"__builtins__": SAFE_BUILTINS}
    env.update(cad.API)
    m = cad.Model()
    try:
        exec(compile(src, "model.py", "exec"), env)
        env["build"](m)
    except Exception as e:  # スクリプト側の誤り = LLM に返す
        tb = traceback.extract_tb(e.__traceback__)
        lines = src.splitlines()
        where = [f"line {fr.lineno}: {lines[fr.lineno - 1].strip() if 0 < fr.lineno <= len(lines) else ''}"
                 for fr in tb if fr.filename == "model.py"]
        return {"ok": False, "error": f"{type(e).__name__}: {e}" + ("\n  at " + where[-1] if where else "")}
    if not any(p.shapes for p in m.parts.values()):
        return {"ok": False, "error": "build(m) did not add any shape (call m.add(shape, material) "
                                      "or part.add(shape, material))"}
    base = os.path.splitext(out_glb)[0]
    try:
        chk = m.check()
        joint_axes = moved = None
        if m.is_assembly():
            name = os.path.basename(base)
            folder = base
            rel = os.path.relpath(folder, os.path.join(REPO, "assets")).replace(os.sep, "/")
            scene, xml, info = m.export_assembly(folder, name, rel, height=height, layer=layer)
            with open(base + ".xml", "w", encoding="utf-8") as f:
                f.write(xml)
            info.update({"kind": "assembly", "folder": folder, "xml": base + ".xml",
                         "outside_assets": rel.startswith("..")})
            joint_axes = info.pop("joint_axes")
            moved = info.pop("moved_scene")
            if os.path.isfile(out_glb):  # 前に 1 つの .glb で作っていたら消す（取り違え防止）
                os.remove(out_glb)
        else:
            scene, info = m.export(out_glb, height=height, origin=origin)
            info.update({"kind": "single", "glb": out_glb})
    except Exception as e:
        return {"ok": False, "error": f"export failed: {type(e).__name__}: {e}"}
    info.update(chk)
    info["ok"] = True
    if png:
        try:
            cad.preview_png(scene, png, joint_axes=joint_axes, moved=moved)
            info["png"] = png
        except Exception as e:  # 絵は無くてもよい
            info["png_error"] = f"{type(e).__name__}: {e}"
    return info


def limit_resources():
    try:
        import resource
        gb = 4 * 1024 ** 3
        resource.setrlimit(resource.RLIMIT_AS, (gb, gb))
    except Exception:
        pass  # Windows には resource が無い（時間切れだけで守る）


def run_script(path, out_glb, args, png):
    """別プロセスで --script を回して結果の dict を返す。"""
    with tempfile.NamedTemporaryFile("r", suffix=".json", delete=False) as tf:
        report = tf.name
    cmd = [sys.executable, os.path.abspath(__file__), "--script", path, "-o", out_glb,
           "--origin", args.origin, "--layer", str(args.layer), "--report", report]
    if args.height:
        cmd += ["--height", str(args.height)]
    if not png:
        cmd += ["--no-png"]
    try:
        kw = {}
        if os.name == "posix":
            kw["preexec_fn"] = limit_resources
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout, **kw)
        try:
            with open(report, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            tail = (p.stderr or p.stdout or "").strip().splitlines()[-5:]
            return {"ok": False, "error": "the script crashed: " + " | ".join(tail)}
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"the script took longer than {args.timeout} s (too many shapes or segments?)"}
    finally:
        try:
            os.remove(report)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# 本体
# ---------------------------------------------------------------------------


def slugify(text):
    s = re.sub(r"[^A-Za-z0-9]+", "_", text.lower()).strip("_")
    return (s[:40].rstrip("_") or "model")


def describe(info):
    sz = info.get("size", [0, 0, 0])
    s = f"{sz[0]:.3g} x {sz[1]:.3g} x {sz[2]:.3g} m, "
    if info.get("kind") == "assembly":
        s += f"{info.get('parts')} parts / {info.get('joints')} joints, "
    s += (f"{info.get('shapes')} shapes, {info.get('materials')} materials, "
          f"{info.get('triangles')} triangles, {info.get('bytes', 0) / 1024:.0f} KB")
    if info.get("pieces", 1) > 1:
        s += f", {info['pieces'] - 1} loose pieces"
    if info.get("problems"):
        s += f", {len(info['problems'])} problems"
    return s


def feedback(info):
    """LLM に返す文（無ければ None）。浮いた形と、組み立ての誤り。"""
    lines = []
    if info.get("pieces", 1) > 1:
        lines.append("Some shapes float apart from the rest of their part (shapes of one part must overlap "
                     "by 2-5 mm). Bounding boxes (min x y z, max x y z) of the separate pieces, largest "
                     "first per part:")
        for b in info.get("pieces_bounds", [])[:12]:
            if isinstance(b[0], str):
                lines.append(f"- part {b[0]!r}: {b[1:4]} .. {b[4:7]}")
            else:
                lines.append(f"- {b[:3]} .. {b[3:]}")
    for p in info.get("problems", []):
        lines.append("- " + p)
    return "\n".join(lines) if lines else None


def generate(args, llm):
    os.makedirs(args.out_dir, exist_ok=True)
    name = args.name or slugify(args.prompt)
    base = os.path.join(args.out_dir, name)
    glb, py, png, log = base + ".glb", base + ".py", base + ".png", base + ".chat.txt"

    messages = [{"role": "system", "content": system_prompt()},
                {"role": "user", "content": user_prompt(args.prompt, args.height, args.parts)}]
    print(f"model: {llm['model']} @ {llm['url']}" + (f"  (config: {llm['_from']})" if llm.get("_from") else ""))
    best = None
    for attempt in range(args.repairs + 1):
        print(f"[{attempt + 1}/{args.repairs + 1}] asking the LLM...")
        reply = chat(llm, messages)
        messages.append({"role": "assistant", "content": reply})
        code = extract_code(reply)
        if code is None:
            problem = "Your reply contains no ```python code block with def build(m)."
            result = {"ok": False, "error": problem}
        else:
            header = (f"# {args.prompt.strip()}\n# generated by samples/meshgen/meshgen.py "
                      f"(model {llm['model']}, attempt {attempt + 1})\n")
            with open(py, "w", encoding="utf-8") as f:
                f.write(header + code)
            result = run_script(py, glb, args, None if args.no_png else png)
        save_log(log, messages, result)
        if result.get("ok"):
            print("  built: " + describe(result))
            best = result
            problem = feedback(result)
            if problem is None or attempt == args.repairs:
                break
            print("  problem: " + problem.replace("\n", "\n           "))
        else:
            problem = result["error"]
            print("  problem: " + problem.replace("\n", "\n           "))
            if attempt == args.repairs:
                break
        messages.append({"role": "user", "content": problem + "\n\nFix it and reply with the complete "
                                                    "corrected script in one ```python block."})
    if not best:
        sys.exit(f"failed: the LLM did not produce a working script (see {log})")
    if feedback(best):
        print("\nwarning: the last version still has problems:\n" + feedback(best))
    report_outputs(best, name, extra=[f"{py}  (edit and rebuild with --script)", log], scene=args.scene)


def report_outputs(info, name, extra=(), scene=False):
    """書いたファイルと、シーンでの使い方。"""
    print()
    if info.get("kind") == "assembly":
        print(f"wrote {info['folder']}{os.sep}  ({info['parts']} part .glb files)")
        for p in info.get("part_info", []):
            print(f"        {p['name']}.glb  {p['mass']:g} kg" + ("  fixed" if p["fixed"] else ""))
        print(f"      {info['xml']}  (scene document: bodies + joints)")
    else:
        print(f"wrote {info['glb']}")
    for e in list(extra) + ([info["png"]] if info.get("png") else []):
        print(f"      {e}")
    if info.get("kind") == "assembly":
        if info.get("outside_assets"):
            print("\nnote: the output is outside assets/, so the engine cannot load the <mesh file> paths. "
                  "Use --out-dir under assets/ (the default).")
            return
        if scene:
            dst = install_scene(info["xml"], name)
            if dst:
                print(f"\nscene: {dst}  (double-click its tile in the asset panel to open it)")
                return
        print("\nuse it: paste the .xml into the browser's 📄 XML editor and apply it, or pass --scene to "
              "put it in assets/scenes/.")
        return
    try:
        rel = os.path.relpath(info["glb"], os.path.join(REPO, "assets")).replace(os.sep, "/")
    except ValueError:
        return
    if rel.startswith(".."):
        return
    h = info.get("size", [0, 0, 0])[1]
    r = max(info.get("size", [0.1, 0.1, 0.1])) / 2
    print("\nscene XML:")
    print(f'  <mesh name="{name}" file="{rel}" scale="1"/>        (in <asset>)')
    print(f'  <body name="{name}1" pos="0 {h / 2 + 0.01:.3g} 0">')
    print(f'    <geom type="mesh" mesh="{name}" size="{r:.3g}" mass="1"/>')
    print("  </body>")


def install_scene(xml_path, name):
    """assets/scenes/<name>.xml へ置く。meshgen が作ったもの以外は上書きしない。"""
    dst = os.path.join(REPO, "assets", "scenes", name + ".xml")
    if os.path.isfile(dst):
        with open(dst, encoding="utf-8") as f:
            if "generated by samples/meshgen" not in f.read():
                print(f"\nnot written: {dst} exists and was not made by meshgen (choose another --name)")
                return None
    with open(xml_path, encoding="utf-8") as src, open(dst, "w", encoding="utf-8") as out:
        out.write(src.read())
    return dst


def save_log(path, messages, result):
    with open(path, "w", encoding="utf-8") as f:
        for m in messages:
            f.write(f"\n======== {m['role']} ({len(m['content'])} chars) ========\n{m['content']}\n")
        f.write("\n======== result ========\n" + json.dumps(result, indent=2, ensure_ascii=False) + "\n")


def build_only(args):
    """--script: LLM を使わずにスクリプトから作る。"""
    out = args.output or os.path.splitext(args.script)[0] + ".glb"
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    png = None if args.no_png else os.path.splitext(out)[0] + ".png"
    if args.report:  # 子プロセスとして呼ばれた
        res = run_script_inprocess(args.script, out, args.height, args.origin, png, args.layer)
        with open(args.report, "w", encoding="utf-8") as f:
            json.dump(res, f)
        return
    res = run_script(args.script, out, args, png)
    if not res.get("ok"):
        sys.exit("failed: " + res["error"])
    print("built: " + describe(res))
    if feedback(res):
        print("warning:\n" + feedback(res))
    report_outputs(res, slugify(os.path.splitext(os.path.basename(out))[0]), scene=args.scene)


def main():
    if hasattr(__import__("signal"), "SIGPIPE"):  # `| head` で切られても例外を出さずに終わる
        import signal
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    ap = argparse.ArgumentParser(
        description="Generate a .glb locally: an LLM writes a CSG script (cad.py API) that is run here.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            examples:
              python3 meshgen.py "a wooden dining chair" --height 0.9
              python3 meshgen.py "a kitchen cabinet with two doors and a drawer" --parts --scene
              python3 meshgen.py --script examples/chair.py --height 0.9
              python3 meshgen.py --script examples/cabinet.py -o ../../assets/generated/local/cabinet.glb --scene
            """))
    ap.add_argument("prompt", nargs="?", help="what to build (any language)")
    ap.add_argument("--height", type=float, default=None,
                    help="scale the result so that its height (Y) is exactly this many meters")
    ap.add_argument("--name", help="output file name without extension (default: from the prompt)")
    ap.add_argument("--out-dir", default=DEFAULT_OUT, help=f"output folder (default: {DEFAULT_OUT})")
    ap.add_argument("--parts", action="store_true",
                    help="ask the LLM to split the object into moving parts connected by joints")
    ap.add_argument("--scene", action="store_true",
                    help="for an assembly: also put the scene document in assets/scenes/<name>.xml")
    ap.add_argument("--layer", type=int, default=7, choices=range(8), metavar="0-7",
                    help="collision layer of an assembly's parts; they do not collide with each other "
                         "(default 7)")
    ap.add_argument("--origin", choices=("center", "bottom"), default="center",
                    help="single model origin: bounding-box center (default, = engine body pos) or bottom "
                         "center. Assembly parts always use their convex-hull centroid")
    ap.add_argument("--repairs", type=int, default=3, help="how many times the LLM may fix its script")
    ap.add_argument("--timeout", type=int, default=120, help="seconds a script may run")
    ap.add_argument("--no-png", action="store_true", help="do not write the preview image")
    ap.add_argument("--config", help="config.json to read ai.llm from")
    ap.add_argument("--url", help="override ai.llm.url")
    ap.add_argument("--model", help="override ai.llm.model")
    ap.add_argument("--script", help="build from this script instead of asking the LLM")
    ap.add_argument("-o", "--output", help="with --script: output .glb (default: next to the script)")
    ap.add_argument("--report", help=argparse.SUPPRESS)  # 子プロセス用
    ap.add_argument("--print-prompt", action="store_true", help="print the system prompt and exit")
    args = ap.parse_args()

    if args.print_prompt:
        print(system_prompt())
        return
    if args.script:
        build_only(args)
        return
    if not args.prompt:
        ap.error("give a prompt, or --script FILE")
    llm = load_llm_config(args.config)
    if args.url:
        llm["url"] = args.url
    if args.model:
        llm["model"] = args.model
    llm.setdefault("url", "https://api.openai.com/v1")
    if not llm.get("apiKey"):
        llm["apiKey"] = os.environ.get("WIZ_LLM_API_KEY") or os.environ.get("OPENAI_API_KEY") or ""
    if not llm.get("model"):
        sys.exit("no LLM model: set ai.llm.model in config.json or pass --model "
                 "(e.g. --url http://127.0.0.1:11434/v1 --model qwen2.5-coder:14b for Ollama)")
    try:
        generate(args, llm)
    except RuntimeError as e:
        sys.exit(f"failed: {e}")


if __name__ == "__main__":
    main()
