"""The game's material table: pass -> shaders, textures, parameters.

Step 0 gave a draw its shader names. This gives those names a meaning, because a
`.material` says what a pass IS: which vertex and pixel shader it runs, which
textures fill its sampler slots IN ORDER, and what its constants are called.

    <Pass>
      <Shaders>
        <VertexShader asset="Template_ATV_MX_DifSpecAlpha"
                      entryPoint="StaticVertexShader"
                      skinningEntryPoint="SkinnedVertexShader"/>
        <PixelShader  asset="Template_ATV_MX_DifSpecAlpha"
                      entryPoint="SharedPixelShader"/>
      </Shaders>
      <Textures numTextures="2">
        <Texture textureName="ATV_Alpha"/>
        <Texture textureName="ATV_Alpha_Combo"/>
      </Textures>
      <Parameters>
        <Parameter name="usr_specular_hardness" type="Constant"/>
      </Parameters>
      <XenonStates numStates="11"/>
    </Pass>

The shader reference is `asset` + `entryPoint`, which is EXACTLY the
`<asset>::<EntryPoint>` identity tools/shader_manifest.py already resolves at
runtime. So the two tables join with no new machinery.

TWO THINGS THIS DOES NOT GIVE YOU, both measured rather than assumed:

  * NO RENDER STATE. `<XenonStates numStates="11"/>` is a count. The binary
    holds attribute counts only -- after the string table it reads 2,2,7,7,11
    per pass and nothing else -- so the eleven state VALUES are not in the file.
    Output-merger state has to come from somewhere else.

  * A SHADER PAIR DOES NOT IDENTIFY A MATERIAL. There are only ~332 distinct
    pairs across ~10185 passes, and the worst is shared by 321 materials. Use
    this table to say what a pass DOES; do not use it to say which material a
    draw is. That needs the guest's own material name.

`skinningEntryPoint` matters: a pass has two possible vertex entry points and
the runtime may run either, so both are indexed.

Usage:
    py -3 tools/material_table.py --assets out/all
"""

import argparse
import collections
import glob
import json
import os
import re
import sys

PASS_RE = re.compile(r"<Pass>(.*?)</Pass>", re.S)
VS_RE = re.compile(r'<VertexShader\s+asset="([^"]*)"\s+entryPoint="([^"]*)"'
                   r'(?:\s+skinningEntryPoint="([^"]*)")?')
PS_RE = re.compile(r'<PixelShader\s+asset="([^"]*)"\s+entryPoint="([^"]*)"')
TEX_RE = re.compile(r'textureName="([^"]*)"')
PARAM_RE = re.compile(r'<Parameter\s+name="([^"]*)"\s+type="([^"]*)"')
STATES_RE = re.compile(r'<XenonStates\s+numStates="(\d+)"')


def read_material(path):
    """Passes of one material .xml, or None if it is not a material."""
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    if "MaterialEditorMaterial" not in text:
        return None
    passes = []
    for block in PASS_RE.findall(text):
        vs = VS_RE.search(block)
        ps = PS_RE.search(block)
        if not (vs and ps):
            continue
        passes.append({
            "vs_asset": vs.group(1),
            "vs_entry": vs.group(2),
            "vs_skinned_entry": vs.group(3) or "",
            "ps_asset": ps.group(1),
            "ps_entry": ps.group(2),
            "textures": TEX_RE.findall(block),
            "parameters": [{"name": n, "type": t}
                           for n, t in PARAM_RE.findall(block)],
            "num_states": int(STATES_RE.search(block).group(1))
            if STATES_RE.search(block) else 0,
        })
    return passes


def shader_labels(p):
    """The `asset::entry` labels this pass can present at runtime.

    Two vertex labels when the pass declares a skinning entry point, because the
    guest picks between them and the runtime only sees which one ran.
    """
    vs = [f"{p['vs_asset']}::{p['vs_entry']}"]
    if p["vs_skinned_entry"]:
        vs.append(f"{p['vs_asset']}::{p['vs_skinned_entry']}")
    return vs, f"{p['ps_asset']}::{p['ps_entry']}"


def main():
    ap = argparse.ArgumentParser(
        description="Build the material table from the extracted assets")
    ap.add_argument("--assets", default="out/all")
    ap.add_argument("--out", default="out/material_table.json")
    ap.add_argument("--index", default="userdata/material_index.txt",
                    help="flat vs+ps -> materials map, for the runtime")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.assets, "**", "*.xml"),
                             recursive=True))
    if not paths:
        sys.exit(f"no .xml under {args.assets}")

    materials = {}
    name_to_paths = collections.defaultdict(list)
    pair_to_materials = collections.defaultdict(set)
    n_pass = 0
    n_files = 0
    tex_hist = collections.Counter()
    state_hist = collections.Counter()

    for path in paths:
        passes = read_material(path)
        if not passes:
            continue
        n_files += 1
        # KEYED BY PATH, NOT BY NAME. Material names are NOT unique across
        # packages -- 5339 files carry only 2665 distinct basenames, and
        # `HFT_Deform_Default` alone appears 20 times. Keying by name silently
        # dropped half the table, and the tell was only that the material count
        # came out at 2654 when the file count is 5339.
        #
        # This matters beyond the table: the guest looks a material up BY NAME
        # (sub_82388560), so a name the runtime hands us is not an identity
        # either. Whatever consumes this has to carry the package too.
        key = os.path.relpath(path, args.assets).replace(os.sep, "/")
        key = os.path.splitext(key)[0]
        materials[key] = passes
        name_to_paths[os.path.basename(key)].append(key)
        name = key
        for p in passes:
            n_pass += 1
            tex_hist[len(p["textures"])] += 1
            state_hist[p["num_states"]] += 1
            vs_labels, ps_label = shader_labels(p)
            for vs_label in vs_labels:
                pair_to_materials[(vs_label, ps_label)].add(name)

    dupe_names = {n: p for n, p in name_to_paths.items() if len(p) > 1}
    print(f"material files {n_files}")
    print(f"materials      {len(materials)}   (keyed by path)")
    print(f"distinct NAMES {len(name_to_paths)}   "
          f"-- {len(dupe_names)} of them used by more than one package")
    print(f"passes         {n_pass}")
    print(f"distinct pairs {len(pair_to_materials)}   "
          f"(counting skinned vertex entry points as their own label)")

    sizes = collections.Counter(len(v) for v in pair_to_materials.values())
    unique = sizes.get(1, 0)
    print(f"pairs naming exactly one material: {unique} "
          f"({unique * 100.0 / max(len(pair_to_materials), 1):.1f}%)")
    print(f"worst ambiguity: {max(sizes)} materials share one pair")
    print(f"textures per pass: {dict(sorted(tex_hist.items()))}")
    print(f"XenonStates counts seen: {dict(sorted(state_hist.items()))}")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(materials, f, indent=1, sort_keys=True)

    # A DIAGNOSTIC index, not an identification table. Each line is a shader
    # pair, how many materials share it, and a few examples -- capped, because
    # one pair covers 321 materials and a file full of those is unreadable
    # without being any more useful. Anything that needs the full set reads the
    # JSON.
    #
    # The point of this file is to make the ambiguity visible: a pair naming 56
    # boot materials that differ only by texture is why a shader pair cannot
    # tell you which material a draw is.
    os.makedirs(os.path.dirname(args.index) or ".", exist_ok=True)
    with open(args.index, "w", encoding="utf-8") as f:
        for (vs, ps) in sorted(pair_to_materials):
            mats = sorted(pair_to_materials[(vs, ps)])
            shown = ",".join(mats[:4])
            more = f",+{len(mats) - 4} more" if len(mats) > 4 else ""
            f.write("%s\t%s\t%d\t%s%s\n" % (vs, ps, len(mats), shown, more))

    print(f"\nwrote {args.out}   "
          f"({os.path.getsize(args.out) / 1e3:.0f} KB)")
    print(f"wrote {args.index}   "
          f"({os.path.getsize(args.index) / 1e3:.0f} KB)")


if __name__ == "__main__":
    main()
