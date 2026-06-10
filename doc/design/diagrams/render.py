#!/usr/bin/env python3
"""Generates the doc/design/ architecture diagrams.

Every diagram in the design docs is produced by this script — never
hand-edited — so the pictures can be regenerated when the architecture
moves. Requires: python3 with `matplotlib` and `graphviz` (pip), plus
the `dot` binary (brew install graphviz).

Usage:  python3 doc/design/diagrams/render.py   # writes *.svg here

House style: graphviz for graph-shaped diagrams (auto-layout),
matplotlib for layout maps (manual placement), one shared palette.
"""
import os

OUT = os.path.dirname(os.path.abspath(__file__))

# Shared palette.
SKY, AMBER, GREEN = '#0EA5E9', '#F59E0B', '#22C55E'
COMPILE_RAMP = ['#6366F1', '#7C3AED', '#8B5CF6', '#A855F7', '#C026D3']
INDIGO, VIOLET, PURPLE, MAGENTA = COMPILE_RAMP[0], COMPILE_RAMP[1], COMPILE_RAMP[3], COMPILE_RAMP[4]
TEAL, EMERALD = '#10B981', '#059669'
SLATE, RED = '#64748B', '#EF4444'


def pipeline():
    """The compile->eval pipeline: the system in one picture."""
    import graphviz
    g = graphviz.Digraph('pipeline', format='svg')
    g.attr(rankdir='LR', bgcolor='white', pad='0.4', fontname='Helvetica',
           splines='spline', nodesep='0.45', ranksep='0.6')
    g.attr('node', fontname='Helvetica', style='filled,rounded', shape='box',
           color='none', fontcolor='white', margin='0.22,0.16', fontsize='12')
    g.attr('edge', fontname='Helvetica', fontsize='10', color=SLATE,
           penwidth='1.6', arrowsize='0.8')
    g.node('src', 'CEL source\n"resource.owner == claims.sub"', fillcolor=SKY)
    with g.subgraph(name='cluster_compile') as c:
        c.attr(label='compile time — celwasm::Compiler', style='rounded,filled',
               fillcolor='#EEF2FF', color=INDIGO, fontcolor='#3730A3', fontsize='13')
        names = [('parse', 'parse + check\n(cel-cpp)'), ('resolve', 'resolve pass'),
                 ('layout', 'layout pass\nrodata · slots'),
                 ('lower', 'lower\n(Binaryen IR)'), ('opt', 'optimize\n+ emit')]
        for (n, lbl), fc in zip(names, COMPILE_RAMP):
            c.node(n, lbl, fillcolor=fc)
        for (a, _), (b, _) in zip(names, names[1:]):
            c.edge(a, b)
    g.node('prog', 'Program\nwasm bytes + cel.abi', fillcolor=AMBER, fontcolor='#451A03')
    with g.subgraph(name='cluster_eval') as c:
        c.attr(label='run time — wasmtime sandbox', style='rounded,filled,dashed',
               fillcolor='#ECFDF5', color=TEAL, fontcolor='#065F46', fontsize='13')
        c.node('plan', 'Engine::Plan\n(Cranelift JIT)', fillcolor=TEAL)
        c.node('inst', 'Instance::Eval', fillcolor=EMERALD)
        c.edge('plan', 'inst')
    g.node('act', 'Activation\nresource=…  claims=…', fillcolor=SKY)
    g.node('val', 'Value\ntrue', fillcolor=GREEN, fontcolor='#052E16')
    g.edge('src', 'parse')
    g.edge('opt', 'prog')
    g.edge('prog', 'plan', label=' portable — ship anywhere ')
    g.edge('act', 'inst')
    g.edge('inst', 'val')
    g.render(f'{OUT}/pipeline', cleanup=True)


def dependency_graph():
    """First-party package dependencies (verified BUILD edges)."""
    import graphviz
    g = graphviz.Digraph('deps', format='svg')
    g.attr(rankdir='TB', bgcolor='white', pad='0.4', fontname='Helvetica',
           splines='spline', nodesep='0.4', ranksep='0.7')
    g.attr('node', fontname='Helvetica', style='filled,rounded', shape='box',
           color='none', fontcolor='white', margin='0.2,0.13', fontsize='11')
    g.attr('edge', fontname='Helvetica', fontsize='9', color=SLATE,
           penwidth='1.4', arrowsize='0.7')
    with g.subgraph(name='cluster_compiler') as c:
        c.attr(label='compiler/  (wasm-targetable — never depends on eval/)',
               style='rounded,filled', fillcolor='#EEF2FF', color=INDIGO,
               fontcolor='#3730A3', fontsize='12')
        c.node('compiler_pub', 'compiler\n(public: Compiler · Program)', fillcolor=INDIGO)
        c.node('frontend', 'frontend\nparse_and_check', fillcolor=VIOLET)
        c.node('ir', 'ir\ntyped_ast · annotations', fillcolor='#A855F7')
        c.node('codegen', 'codegen\nresolve · layout · lower', fillcolor=MAGENTA)
        c.node('celfn', 'celfn\nFunctionLibrary · emitters', fillcolor='#DB2777')
    with g.subgraph(name='cluster_eval') as c:
        c.attr(label='eval/  (host-side evaluator)', style='rounded,filled',
               fillcolor='#ECFDF5', color=TEAL, fontcolor='#065F46', fontsize='12')
        c.node('eval_pub', 'eval\n(public: Engine · Instance · Activation · Value)',
               fillcolor=TEAL)
        c.node('eval_int', 'eval/internal\ncel_host · abi_decode · wasmtime glue',
               fillcolor=EMERALD)
    g.node('shared', 'shared\nCelType', fillcolor=SKY)
    g.node('abi', 'abi\ncel.abi emit · runtime catalogue', fillcolor='#0284C7')
    g.node('runtime', 'runtime\ncel_runtime.c → .wasm kernel', fillcolor=AMBER,
           fontcolor='#451A03')
    for a, b in [('compiler_pub', 'frontend'), ('compiler_pub', 'celfn'),
                 ('frontend', 'ir'), ('codegen', 'ir'), ('compiler_pub', 'codegen'),
                 ('frontend', 'shared'), ('compiler_pub', 'shared'),
                 ('codegen', 'abi'), ('codegen', 'runtime'),
                 ('eval_pub', 'compiler_pub'), ('eval_pub', 'celfn'),
                 ('eval_pub', 'eval_int'), ('eval_pub', 'runtime'),
                 ('eval_int', 'abi'), ('eval_int', 'ir'), ('eval_pub', 'shared')]:
        g.edge(a, b)
    g.render(f'{OUT}/dependency-graph', cleanup=True)


def trust_boundary():
    """Host process vs wasmtime sandbox vs component sandbox."""
    import graphviz
    g = graphviz.Digraph('trust', format='svg')
    g.attr(rankdir='LR', bgcolor='white', pad='0.45', fontname='Helvetica',
           splines='spline', nodesep='0.5', ranksep='1.0')
    g.attr('node', fontname='Helvetica', style='filled,rounded', shape='box',
           color='none', fontcolor='white', margin='0.22,0.15', fontsize='11')
    g.attr('edge', fontname='Helvetica', fontsize='9.5', color=SLATE,
           penwidth='1.5', arrowsize='0.75')
    with g.subgraph(name='cluster_host') as c:
        c.attr(label='host process (trusted)', style='rounded,filled',
               fillcolor='#F1F5F9', color='#475569', fontcolor='#0F172A', fontsize='13')
        c.node('app', 'your application\nprotos · policy data', fillcolor='#334155')
        c.node('engine', 'Engine / Instance\n(wasmtime embedder)', fillcolor=TEAL)
        c.node('hostfn', '@host functions\nrun in-process, full trust', fillcolor=SKY)
    with g.subgraph(name='cluster_sandbox') as c:
        c.attr(label='wasmtime sandbox — no syscalls, no ambient authority',
               style='rounded,filled,dashed', fillcolor='#ECFDF5', color=TEAL,
               fontcolor='#065F46', fontsize='13')
        c.node('expr', 'compiled expression\n(your CEL, as wasm)', fillcolor=MAGENTA)
        c.node('kernel', 'cel_runtime.wasm kernel\nshared linear memory',
               fillcolor=AMBER, fontcolor='#451A03')
        c.edge('expr', 'kernel', label='cel.* imports')
    with g.subgraph(name='cluster_component') as c:
        c.attr(label='component sandbox (own memory)', style='rounded,filled,dashed',
               fillcolor='#FDF4FF', color=MAGENTA, fontcolor='#701A75', fontsize='13')
        c.node('comp', '@component functions\nuntrusted custom fns', fillcolor=PURPLE)
    g.edge('app', 'engine')
    g.edge('engine', 'expr', label='Plan / Eval')
    g.edge('expr', 'hostfn', label='cel_host imports\n(mediated trampolines)',
           dir='both', color='#DC2626', fontcolor='#DC2626')
    g.edge('expr', 'comp', label='canonical-ABI marshal\n(host-mediated)',
           dir='both', color='#9333EA', fontcolor='#9333EA')
    g.render(f'{OUT}/trust-boundary', cleanup=True)


def memory_map():
    """The one shared linear memory, regions + lifetimes + CelValue cell."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.patches import FancyBboxPatch, Rectangle

    fig, ax = plt.subplots(figsize=(16, 6.2))
    ax.set_xlim(0, 16); ax.set_ylim(0, 6.2); ax.axis('off')
    ax.text(0.1, 5.85, 'One shared linear memory per Instance', fontsize=15,
            weight='bold', color='#0F172A')
    ax.text(0.1, 5.5, 'defined + exported shared by cel_runtime.wasm — '
            '(memory 4 1024 shared), init 256 KiB, max 64 MiB; '
            'expr module imports cel.memory', fontsize=9.5, color='#475569')

    BAR_Y, BAR_H = 3.1, 1.5

    def region(x, w, fc, label, sub='', tc='white', fs=9.5):
        ax.add_patch(Rectangle((x, BAR_Y), w, BAR_H, fc=fc, ec='white', lw=1.2))
        ax.text(x + w/2, BAR_Y + BAR_H*0.62, label, ha='center', va='center',
                color=tc, fontsize=fs, weight='bold')
        if sub:
            ax.text(x + w/2, BAR_Y + BAR_H*0.28, sub, ha='center', va='center',
                    color=tc, fontsize=7.5)

    def tick(x, label, weight='normal'):
        ax.plot([x, x], [BAR_Y - 0.18, BAR_Y], color='#0F172A', lw=1)
        ax.text(x, BAR_Y - 0.32, label, ha='center', fontsize=8.5,
                color='#0F172A', weight=weight)

    def lifetime(x, w, label, color):
        ax.add_patch(Rectangle((x, BAR_Y + BAR_H + 0.12), w, 0.34, fc=color,
                     ec='none', alpha=0.18))
        ax.text(x + w/2, BAR_Y + BAR_H + 0.29, f'lifetime: {label}',
                ha='center', va='center', fontsize=8.5, color='#0F172A',
                style='italic')

    region(0.1, 0.42, '#0F172A', '', '')
    ax.text(0.31, BAR_Y + BAR_H*0.45, 'null\nsentinel', ha='center', va='center',
            color='white', fontsize=6.5, weight='bold')
    region(0.52, 2.5, INDIGO, '.rodata',
           'const CelValue headers\n+ string/bytes payloads')
    region(3.02, 2.9, VIOLET, 'workspace slots',
           '24-byte CelValue cells — vars + scratch')
    region(5.92, 3.1, '#94A3B8', 'wasi-libc statics\n+ 64 KiB shadow stack',
           'cel_runtime.wasm internals\noff-limits to codegen', fs=8)
    region(9.02, 6.6, '#FDE68A', '', '', tc='#451A03')
    ax.text(9.32, BAR_Y + BAR_H - 0.22, 'dlmalloc heap', fontsize=10,
            weight='bold', color='#451A03')
    for (x, w, fc, l1, l2) in [
            (9.25, 1.9, RED, 'per-Eval bump arena',
             '64 KiB, malloc once/Instance\narena_reset between Evals'),
            (11.35, 1.9, SKY, 'activation buffer',
             'per-Instance, realloc-grown\nbound string/bytes payloads'),
            (13.45, 1.9, TEAL, 'Plan-lifetime objects',
             'RE2 cache, parsed\ntimestamps, …')]:
        ax.add_patch(FancyBboxPatch((x, BAR_Y + 0.12), w, BAR_H - 0.55,
                     boxstyle='round,pad=0.03,rounding_size=0.08', fc=fc, ec='none'))
        ax.text(x + w/2, BAR_Y + 0.72, l1, ha='center', va='center',
                color='white', fontsize=8, weight='bold')
        ax.text(x + w/2, BAR_Y + 0.38, l2, ha='center', va='center',
                color='white', fontsize=6.3)

    tick(0.1, '0', 'bold'); tick(0.52, '16'); tick(3.02, 'ws_base')
    tick(5.92, '8192', 'bold'); tick(9.02, '__heap_base (~243 568)', 'bold')
    ax.text(5.92, BAR_Y - 0.62,
            '--global-base  ·  bounded by ValidateExprStaticRegion (Compile) '
            '+ ValidateAbiSlotExtents (Plan)',
            ha='center', fontsize=7.5, color=RED, weight='bold')
    ax.annotate('', xy=(15.8, BAR_Y + BAR_H/2), xytext=(15.62, BAR_Y + BAR_H/2),
                arrowprops=dict(arrowstyle='-|>', color='#451A03'))
    lifetime(0.1, 5.82, 'the module (active data segments)', INDIGO)
    lifetime(5.92, 3.1, 'process', '#64748B')
    lifetime(9.02, 6.6, 'Eval / Instance / Plan', AMBER)

    ax.text(0.1, 2.0, 'The 24-byte CelValue cell (runtime/cel_data.h — '
            'A1–A4 static_asserts)', fontsize=10.5, weight='bold', color='#0F172A')
    CY = 0.7
    for (x, w, fc, label, off) in [
            (0.1, 1.3, INDIGO, 'kind\nu32', '+0'),
            (1.4, 1.3, '#CBD5E1', '_pad\nu32', '+4'),
            (2.7, 5.2, MAGENTA, 'payload (16 B): scalar inline · '
             'string/bytes {ptr,len} · list/map arena header · '
             'message host handle', '+8')]:
        ax.add_patch(Rectangle((x, CY), w, 0.85, fc=fc, ec='white', lw=1.2))
        ax.text(x + w/2, CY + 0.43, label, ha='center', va='center',
                color='white' if fc != '#CBD5E1' else '#334155',
                fontsize=7.5, weight='bold')
        ax.text(x + 0.06, CY - 0.18, off, fontsize=7.5, color='#0F172A')
    ax.text(8.05, CY - 0.18, '+24', fontsize=7.5, color='#0F172A')
    ax.text(8.3, CY + 0.43, '← slots are i32 byte-offsets of these cells;\n'
            '    helpers are (out_slot, arg_slots…) → void', fontsize=8,
            color='#475569', va='center')
    fig.savefig(f'{OUT}/memory-map.svg', bbox_inches='tight', facecolor='white')


if __name__ == '__main__':
    pipeline()
    dependency_graph()
    trust_boundary()
    memory_map()
    print('rendered:', sorted(f for f in os.listdir(OUT) if f.endswith('.svg')))
