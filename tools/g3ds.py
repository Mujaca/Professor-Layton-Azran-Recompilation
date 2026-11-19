#!/usr/bin/env python3
"""Einstiegspunkt: python tools/g3ds.py <unterkommando> ..."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from g3ds.cli import main

if __name__ == "__main__":
    sys.exit(main())
