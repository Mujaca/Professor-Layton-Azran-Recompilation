"""Erlaubt pyctr ohne boot9.bin/Keys, wenn das Image bereits entschluesselt ist."""
import sys
from collections import defaultdict

class FakeCrypto:
    key_normal = defaultdict(lambda: bytes(16))
    key_x = defaultdict(lambda: bytes(16))

    def __init__(self, *a, **k):
        pass

    def set_keyslot(self, *a, **k):
        pass

    def set_normal_key(self, *a, **k):
        pass

    def create_ctr_io(self, *a, **k):
        raise RuntimeError("FakeCrypto: Image scheint nicht entschluesselt zu sein")

    def create_ctr_cipher(self, *a, **k):
        raise RuntimeError("FakeCrypto: Image scheint nicht entschluesselt zu sein")

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def install():
    import pyctr  # noqa
    import pyctr.crypto  # noqa
    for name, mod in list(sys.modules.items()):
        if name.startswith('pyctr') and hasattr(mod, 'CryptoEngine'):
            setattr(mod, 'CryptoEngine', FakeCrypto)
    return FakeCrypto
