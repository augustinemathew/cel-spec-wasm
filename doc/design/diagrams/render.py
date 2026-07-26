#!/usr/bin/env python3
"""Generates the graphviz-shaped doc/design/ diagrams.

This script owns only `dependency-graph.svg` (auto-layout graph).
The presentation diagrams — `memory-map`, `plan-eval`,
`trust-boundary` (each as a `-light.svg`/`-dark.svg` pair) — are
hand-authored SVGs in the same house style as `doc/img/*.svg`
(GitHub palette, light/dark pairs kept in sync by color mapping);
edit those files directly and regenerate the dark variant by
applying the light→dark palette map. Requires: python3 with
`graphviz` (pip) plus the `dot` binary (brew install graphviz).

Usage:  python3 doc/design/diagrams/render.py   # writes *.svg here
"""
import os

OUT = os.path.dirname(os.path.abspath(__file__))

# Shared palette.
SKY, AMBER, GREEN = '#0EA5E9', '#F59E0B', '#22C55E'
COMPILE_RAMP = ['#6366F1', '#7C3AED', '#8B5CF6', '#A855F7', '#C026D3']
INDIGO, VIOLET, PURPLE, MAGENTA = COMPILE_RAMP[0], COMPILE_RAMP[1], COMPILE_RAMP[3], COMPILE_RAMP[4]
TEAL, EMERALD = '#10B981', '#059669'
SLATE, RED = '#64748B', '#EF4444'



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
    g.node('abi', 'abi\ncel.abi emit · Plugin · wasm_binary\nruntime catalogue', fillcolor='#0284C7')
    g.node('runtime', 'runtime\ncel_runtime.c → .wasm kernel', fillcolor=AMBER,
           fontcolor='#451A03')
    for a, b in [('compiler_pub', 'frontend'), ('compiler_pub', 'celfn'),
                 ('frontend', 'ir'), ('codegen', 'ir'), ('compiler_pub', 'codegen'),
                 ('frontend', 'shared'), ('compiler_pub', 'shared'),
                 ('codegen', 'abi'), ('codegen', 'runtime'),
                 ('compiler_pub', 'abi'),   # Compiler::Builder::Use(const Plugin&)
                 ('eval_pub', 'abi'),       # Engine::Use(const Plugin&)
                 ('abi', 'celfn'),          # Plugin::Load → ParseCelfnSource
                 ('eval_pub', 'compiler_pub'), ('eval_pub', 'celfn'),
                 ('eval_pub', 'eval_int'), ('eval_pub', 'runtime'),
                 ('eval_int', 'abi'), ('eval_int', 'ir'), ('eval_pub', 'shared')]:
        g.edge(a, b)
    g.render(f'{OUT}/dependency-graph', cleanup=True)




if __name__ == '__main__':
    dependency_graph()
    print('rendered:', sorted(f for f in os.listdir(OUT) if f.endswith('.svg')))
