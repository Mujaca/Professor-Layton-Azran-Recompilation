"""Baut und startet tests/pica_selftest.c (PICA-Registermap + Kommandoliste).

  python tools\\test_pica_c.py

Prueft 0x200-Basis, 0x201-Formate, 0x203-Loader-Stride, 0x228/0x22F-Draw,
0x80-Textur-Enable und das Textursampling ohne Gast-Code. Zusaetzlich die
Azahar-Semantik des Kommandostroms: Header-Maske, Consecutive-Writes
(group=0/1), 8-Byte-Ausrichtung, 0x010-Finalize (P3D/Autostop),
Befehlspuffer-Sprung 0x238/0x23A/0x23C und die Abbildung physischer
PICA-Adressen auf den Gast-Linearspeicher.

Vertex-Pipeline (Azahar shader_interpreter.cpp / vertex_loader.cpp):
float24-Uniform-Upload 0x2C0/0x2C1, Programm 0x2CB/0x2CC, Swizzle
0x2D5/0x2D6, Bool-/Int-Uniforms 0x2B0/0x2B1, Eingangs-Mapping 0x2B9/0x2BB,
Ausgangs-Mapping 0x04F/0x050, output_mask 0x2BD, Arithmetik/Swizzle,
LOOP+aL, IFU, Default-Attribute 0x232, attribut_mask, Index-Puffer 0x227
(0x22F/u16) und Immediate-Mode 0x232=0xF.

Rasterizer/Fragment (Azahar renderer_software): Clipping, Viewport 0x41/0x43/
0x68, perspektivkorrekte Interpolation, 12.4-Coverage, TEV 0x0C0..0x0FD.

Texturen (Azahar texture_decode.cpp, sw_texturing.cpp): alle 14 Formate
RGBA8/RGB8/RGB5A1/RGB565/RGBA4/IA8/RG8/I8/A8/IA4/I4/A4/ETC1/ETC1A4 mit
8x8-Morton-Kacheln und Azahar-Kanalreihenfolge, Kachel-Morton-Test,
ETC1-Subtile x/4+2*(y/4) und ETC1A4-Alpha-Shift, Wrap-Modi 0..5
(Border-Farbe), Nearest-Sampling wie Azahar (keine Filterung).

Framebuffer (Azahar sw_framebuffer.cpp, sw_rasterizer.cpp): Farbziel-
Formate 0x117/0x11D/0x11E inkl. Byte-Reihenfolge, alle 16 LogicOps,
Alpha-Test, Blending (Add/SrcAlpha/1-SrcAlpha), Depth-Test/-Write (D24S8),
Stencil-Test und Schreibgrenzen des Farbziels.

Fog (Azahar sw_rasterizer.cpp WriteFog): LUT-Upload 0xE6/0xE8..0xEF
(Offset + Inkrement), fog_mode 0xE0 Bits0..2, fog_flip Bit16, fog_color
0xE1, Index/Fraktion und Mischung vor dem Depth-Test. Texturzustand:
TEXCOORD2 ist Semantic 22/23 (nicht 16/17), und nur Einheit 0 beachtet
den Texturtyp (Bits28..30, 3dbrew/Azahar sw_rasterizer.cpp TextureColor).

Texturtypen (Azahar sw_rasterizer.cpp TextureColor, sw_clipper.cpp
ConvertCubeCoord, output_vertex.cpp): Projection2D teilt u/v durch
TEXCOORD0_W (Semantik 16), Cube/ShadowCube waehlen Face + Koordinaten
ueber die dominante Achse (u'=x/z*0.5+0.5), Shadow2D teilt perspektivisch
und vergleicht z_ref = (a<<16)|(b<<8)|g gegen z_int inkl. Bias 0x8B.

Fragment-Lighting (Azahar sw_lighting.cpp/regs_lighting.h) 0x140..0x1D9:
Lichtquellen (f16-Position, f20-Distanzdaempfung, fix1.1.11-Spot,
10-Bit-Farben), LUT-Upload 0x1C5/0x1C8..0x1CF (Typ/Index + Inkrement),
Distanz-LUT, Fresnel-Alpha, two_sided_diffuse, Clamp und Bump-Mapping
(NormalMap mit/ohne Renorm) sowie Einspeisung als TEV-Quelle 1/2.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'pica_selftest.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime', 'cpu'), '-I', os.path.join(ROOT, 'runtime', 'gpu'),
           os.path.join(ROOT, 'tests', 'pica_selftest.c'),
           os.path.join(ROOT, 'runtime', 'gpu', 'gpu.c'),
           os.path.join(ROOT, 'runtime', 'cpu', 'arm_cpu.c'), '-o', EXE]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print('BUILD-FEHLER:\n' + p.stderr[-3000:])
        return 1
    env = dict(os.environ)
    env['HLE_PICA_RENDER'] = '1'
    r = subprocess.run([EXE], capture_output=True, text=True, env=env)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
