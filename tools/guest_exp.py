#!/usr/bin/env python3
"""Kurzer Einstieg: python tools/guest_exp.py record|list|show|check ..."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from g3ds.cli import main

if __name__ == "__main__":
    sys.exit(main(["guest-exp"] + sys.argv[1:]))
