#!/usr/bin/env python3
"""Kurzer Einstieg: python tools/hotpatch.py <liste|run> [optionen]"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from g3ds.hotpatch import main

if __name__ == "__main__":
    sys.exit(main())
