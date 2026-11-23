"""Mesh-Aufbau aus .prm (XPVB/XPVI) fuer den Titel-3D-Layer."""
import struct

import numpy as np

import title3d_lib as L

# Node-Reihenfolge des Scene-Graphs (aus .mbn-Hashes, CRC32-Namen):
#   title_map
#     title_map_far_01-i600- : m0100-v, m0120-v, m0130-v, m0140-v
#     m0090-                 : additiver Layer (mat.title_map_09_e-add)
#     title_map_gim_01-a742- : m0010, m0020, ..., m0070s (12 Modelle)
# Node-Name -> .prm-Datei (Modellname steht als String im .prm).
NODE_PRM = [
    ('m0100-v', '013.prm'),
    ('m0120-v', '014.prm'),
    ('m0130-v', '015.prm'),
    ('m0140-v', '016.prm'),
    ('m0090', '012.prm'),
    ('m0010-1-241-', '000.prm'),
    ('m0020-242-482-', '002.prm'),
    ('m0030-483-723-', '004.prm'),
    ('m0040-724-725-', '006.prm'),
    ('m0050-726-727-', '008.prm'),
    ('m0010s-728-729-', '001.prm'),
    ('m0020s-730-731-', '003.prm'),
    ('m0030s-732-733-', '005.prm'),
    ('m0040s-734-735-', '007.prm'),
    ('m0050s-736-737-', '009.prm'),
    ('m0060s-738-739-', '010.prm'),
    ('m0070s-740-741-', '011.prm'),
]

# Materialname -> .xi-Index (Reihenfolge der Namenstabelle in RES.bin)
MAT_TEX = {
    'mat.title_map_01_e': 0,
    'mat.title_map_01s_e': 1,
    'mat.title_map_02_e': 2,
    'mat.title_map_02s_e': 3,
    'mat.title_map_03_e': 4,
    'mat.title_map_03s_e': 5,
    'mat.title_map_04_e': 6,
    'mat.title_map_04s_e': 7,
    'mat.title_map_05_e': 8,
    'mat.title_map_05s_e': 9,
    'mat.title_map_06s_e': 10,
    'mat.title_map_07s_e': 11,
    'mat.title_map_09_e-add': 12,
    'mat.title_map_fog01_e-i-add': 13,
    'mat.title_map_fog01_e-i-add-1': 13,
    'mat.title_map_fog01_e-i-add-2': 13,
    'mat.title_map_fog01_e-i-add-3': 13,
}


def prm_names(name):
    b = L.blob(name)
    out = []
    for s in b.split(b'\0'):
        if len(s) >= 4:
            t = s.decode('latin1')
            if t.startswith('mat.') or t.startswith('m0'):
                out.append(t)
    model = next((t for t in out if t.startswith('m0')), None)
    mat = next((t for t in out if t.startswith('mat.')), None)
    return model, mat


def mesh(name):
    """-> list[dict(pos=..., uv=..., shade=...)] als Dreiecke (N,3,...)."""
    d = L.prm_info(name)
    v = d['verts']
    st = d['stride']
    if st == 20:
        pos = v[:, 0:3]
        uv = v[:, 3:5]
        shade = np.ones(len(v), dtype=np.float32)
    else:
        pos = v[:, 0:3]
        shade = v[:, 6]
        uv = v[:, 7:9]
    tris = []
    if st == 20:
        for g in range(len(v) // 4):
            o = g * 4
            # v0=RB v1=LT v2=LB v3=RT  -> Dreiecke (0,1,2) und (1,3,2)
            for a, b, c in ((0, 1, 2), (1, 3, 2)):
                tris.append((pos[o + a], pos[o + b], pos[o + c],
                             uv[o + a], uv[o + b], uv[o + c],
                             shade[o + a], shade[o + b], shade[o + c]))
    else:
        # 9x3-Grid: Spalten = distinkte u, Zeilen = distinkte v
        us = sorted(set(round(float(x), 4) for x in uv[:, 0]))
        vs = sorted(set(round(float(x), 4) for x in uv[:, 1]))
        idxmap = {}
        for i in range(len(v)):
            idxmap[(round(float(uv[i, 0]), 4), round(float(uv[i, 1]), 4))] = i
        for ci in range(len(us) - 1):
            for ri in range(len(vs) - 1):
                q = [(us[ci], vs[ri]), (us[ci + 1], vs[ri]),
                     (us[ci], vs[ri + 1]), (us[ci + 1], vs[ri + 1])]
                try:
                    i0, i1, i2, i3 = (idxmap[x] for x in q)
                except KeyError:
                    continue
                for a, b, c in ((i0, i1, i2), (i1, i3, i2)):
                    tris.append((pos[a], pos[b], pos[c], uv[a], uv[b], uv[c],
                                 shade[a], shade[b], shade[c]))
    return tris
