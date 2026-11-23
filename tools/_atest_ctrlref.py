"""Erzeugt eine Kontroll-Referenz (Inhalt zerstoert, Statistik gleich)."""
import numpy as np

R = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                  dtype=np.uint8).reshape(240, 400, 3)
C = np.roll(R, 203, axis=1)
C = np.roll(C, 37, axis=0)
open('tools/out/atest_control_ref.bin', 'wb').write(C.tobytes())
print('geschrieben tools/out/atest_control_ref.bin', C.shape)
