#!/usr/bin/env python3
"""Architecture diagram generator for PicoSoC / picorv32.

Reads `full_hierarchy.json` (yosys elaboration output) and generates
auto-extracted architecture diagrams as PNG files via graphviz.

Only diagrams actually used in guide.tex are generated:
  10_hierarchy_tree    — module containment tree (used in §picorv32 Microarchitecture)
  23_picosoc_noportcl  — picosoc module interconnect (used in §PicoSoC)

Hand-structured diagrams (PicoSoC architecture, CPU datapath, full system)
are maintained as TikZ in guide.tex directly.

Usage:
    python3 generate.py              # generate diagrams from existing JSON
    python3 generate.py --yosys      # also regenerate full_hierarchy.json from Verilog

Requires: graphviz (dot), yosys (optional, for --yosys)
"""

import json, subprocess, os, sys, tempfile, shutil
from collections import defaultdict

OUTDIR = os.path.dirname(os.path.abspath(__file__))
PICOSOC = os.path.join(OUTDIR, '..', '..', '..', 'picorv32', 'picosoc')
JSON_PATH = os.path.join(OUTDIR, 'full_hierarchy.json')

# ============================================================
# Utilities
# ============================================================

KNOWN_MODULES = {
    'picorv32', 'picosoc', 'ice40up5k_spram', 'simpleuart', 'spimemio',
    'spimemio_xfer', 'picorv32_pcpi_fast_mul', 'picosoc_regs', 'picosoc_mem',
    'seven_seg_ctrl', 'seven_seg_hex', 'icebreaker',
}

COLORS = {
    'picorv32': '#bbdefb', 'picosoc': '#c8e6c9', 'spimemio': '#a5d6a7',
    'simpleuart': '#fff9c4', 'ice40up5k_spram': '#ce93d8',
    'SB_SPRAM256KA': '#e1bee7', 'SB_IO': '#ffe0b2', 'SB_MAC16': '#ffcc80',
    'seven_seg_ctrl': '#ffcc80', 'seven_seg_hex': '#ffe0b2',
    'picorv32_pcpi_fast_mul': '#a5d6a7', 'picosoc_regs': '#90caf9',
    'spimemio_xfer': '#c8e6c9',
}


def clean_name(name):
    """Strip yosys $paramod mangling to get the base module name."""
    for k in sorted(KNOWN_MODULES, key=len, reverse=True):
        if k in name:
            return k
    return name


def sanitize_id(name):
    """Make a name safe for graphviz node IDs."""
    return ''.join(c if c.isalnum() or c == '_' else '_' for c in name)


def render(dot_str, name, dpi=200):
    """Write dot string -> PNG via graphviz."""
    dot_p = os.path.join(OUTDIR, f'{name}.dot')
    png_p = os.path.join(OUTDIR, f'{name}.png')
    with open(dot_p, 'w') as f:
        f.write(dot_str)
    r = subprocess.run(['dot', '-Tpng', f'-Gdpi={dpi}', dot_p, '-o', png_p],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  ERROR {name}: {r.stderr[:300]}")
        return False
    os.remove(dot_p)
    print(f"  {name}.png ({os.path.getsize(png_p) // 1024} KB)")
    return True


def load_json(path):
    with open(path) as f:
        return json.load(f)


def parse_modules(data):
    """Extract module info from yosys JSON. Returns {name: {ports, cells}}."""
    modules = {}
    for mname, mdata in data.get('modules', {}).items():
        cname = clean_name(mname)
        if cname.startswith('SB_') or cname.startswith('ICESTORM'):
            continue
        ports = {}
        for pn, pd in mdata.get('ports', {}).items():
            ports[pn] = {'dir': pd['direction'], 'bits': pd['bits']}
        cells = {}
        for cn, cd in mdata.get('cells', {}).items():
            ct = cd.get('type', '')
            if ct.startswith('$') and '$paramod' not in ct:
                continue
            cells[cn] = {'type': clean_name(ct),
                         'conns': dict(cd.get('connections', {}))}
        modules[cname] = {'ports': ports, 'cells': cells}
    return modules


# ============================================================
# Diagrams
# ============================================================

def diagram_hierarchy_tree(modules):
    """10: Module containment tree — used in guide.tex fig:hierarchy-tree."""
    L = ['digraph {']
    L.append('  label="Module Hierarchy Tree"; labelloc=t; fontsize=16; fontname="Helvetica";')
    L.append('  rankdir=TB; node [fontname="Helvetica", fontsize=11, shape=box, style="filled,rounded"];')

    seen = set()
    def add(mod, parent=None):
        if mod in seen or mod not in modules:
            return
        if mod.startswith('SB_') or mod.startswith('ICESTORM'):
            return
        seen.add(mod)
        nid = sanitize_id(mod)
        nc = len(modules[mod]['cells'])
        np_ = len(modules[mod]['ports'])
        col = COLORS.get(mod, '#f5f5f5')
        L.append(f'  {nid} [label="{mod}\\n({np_} ports, {nc} sub)", fillcolor="{col}"];')
        if parent:
            L.append(f'  {sanitize_id(parent)} -> {nid};')
        for cn, ci in modules[mod]['cells'].items():
            ct = ci['type']
            if ct in modules and not ct.startswith('SB_'):
                add(ct, mod)
            else:
                lid = sanitize_id(f"{mod}_{cn}")
                lc = COLORS.get(ct, '#e0e0e0')
                sh = 'component' if ct.startswith('SB_') else 'box'
                L.append(f'  {lid} [label="{cn}\\n({ct})", fillcolor="{lc}", shape={sh}];')
                L.append(f'  {nid} -> {lid};')

    add('icebreaker')
    L.append('}')
    render('\n'.join(L), '10_hierarchy_tree')


def diagram_module_interconnect(modules, target, title, fname):
    """Auto-extracted block diagram showing submodule instances and shared nets."""
    if target not in modules:
        print(f"  {target} not found"); return
    m = modules[target]
    cells, ports = m['cells'], m['ports']

    net_cells = defaultdict(list)
    for cn, ci in cells.items():
        for pn, bits in ci['conns'].items():
            for b in bits:
                if isinstance(b, int):
                    net_cells[b].append((cn, pn))

    edges = defaultdict(set)
    for net_id, clist in net_cells.items():
        names = list(set(c[0] for c in clist))
        if len(names) >= 2:
            for i in range(len(names)):
                for j in range(i + 1, len(names)):
                    key = tuple(sorted([names[i], names[j]]))
                    p1 = [c[1] for c in clist if c[0] == key[0]]
                    p2 = [c[1] for c in clist if c[0] == key[1]]
                    edges[key].add((p1[0] if p1 else '', p2[0] if p2 else ''))

    L = ['digraph {']
    L.append(f'  label="{title}"; labelloc=t; fontsize=14; fontname="Helvetica-Bold";')
    L.append('  rankdir=LR; nodesep=0.3; ranksep=0.8;')
    L.append('  node [fontname="Helvetica", fontsize=10, style="filled,rounded"];')
    L.append('  edge [fontname="Helvetica", fontsize=8, color="#555555"];')

    for cn, ci in cells.items():
        ct = ci['type']
        col = COLORS.get(ct, '#f5f5f5')
        L.append(f'  {sanitize_id(cn)} [label="{cn}\\n({ct})\\n{len(ci["conns"])} ports", shape=box, fillcolor="{col}", penwidth=1.5];')

    for (c1, c2), ppairs in edges.items():
        labels = set()
        for p1, p2 in ppairs:
            if p1: labels.add(p1)
            if p2: labels.add(p2)
        lbl = ', '.join(sorted(labels)[:4])
        if len(labels) > 4:
            lbl += f' +{len(labels) - 4}'
        L.append(f'  {sanitize_id(c1)} -> {sanitize_id(c2)} [dir=both, label="{lbl}"];')

    L.append('}')
    render('\n'.join(L), fname)


# ============================================================
# Yosys JSON generation
# ============================================================

def regenerate_json():
    """Run yosys to produce full_hierarchy.json from Verilog sources."""
    tmpdir = tempfile.mkdtemp()
    try:
        for f in ['picosoc.v', 'icebreaker.v']:
            src = os.path.join(PICOSOC, f)
            dst = os.path.join(tmpdir, f)
            with open(src) as s, open(dst, 'w') as d:
                d.write(s.read().replace('`error', '// `error'))

        srcs = [
            os.path.join(tmpdir, 'icebreaker.v'),
            os.path.join(tmpdir, 'picosoc.v'),
            os.path.join(PICOSOC, 'simpleuart.v'),
            os.path.join(PICOSOC, 'spimemio.v'),
            os.path.join(PICOSOC, 'ice40up5k_spram.v'),
            os.path.join(PICOSOC, '..', 'picorv32.v'),
        ]
        src_str = ' '.join(srcs)

        cmd = f'''yosys -q -p "
read_verilog {src_str};
read_verilog -lib +/ice40/cells_sim.v;
hierarchy -top icebreaker;
proc; opt_clean;
write_json {JSON_PATH}
"'''
        print("Running yosys...")
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  yosys ERROR: {r.stderr[:500]}")
            return False
        print(f"  full_hierarchy.json ({os.path.getsize(JSON_PATH) // 1024} KB)")
        return True
    finally:
        shutil.rmtree(tmpdir)


# ============================================================
# Main
# ============================================================

if __name__ == '__main__':
    if '--yosys' in sys.argv:
        if not regenerate_json():
            sys.exit(1)

    if not os.path.exists(JSON_PATH):
        print(f"ERROR: {JSON_PATH} not found. Run with --yosys to generate it.")
        sys.exit(1)

    data = load_json(JSON_PATH)
    modules = parse_modules(data)
    print(f"Parsed {len(modules)} modules from yosys JSON\n")

    diagram_hierarchy_tree(modules)
    diagram_module_interconnect(modules, 'picosoc',
                                'picosoc.v — Module Interconnect',
                                '23_picosoc_noportcl')

    print("\nDone! PNGs in:", OUTDIR)
