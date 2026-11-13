#!/usr/bin/env python3
"""ROM-only 007xb asset packer CLI (optional; GUI uses same pipeline)."""
from __future__ import annotations
import argparse
import sys
from pathlib import Path
from xb_studio_pipeline import Pipeline, Settings

def main(argv=None):
    parser=argparse.ArgumentParser(description='Build the single 007xb_assets.dat file from your own GoldenEye ROM')
    parser.add_argument('--rom', required=True, help='Original US GoldenEye .z64, .v64, or .n64')
    args=parser.parse_args(argv)
    root=Path(__file__).resolve().parent.parent
    def log(kind, message):
        if kind == 'log': print(message, flush=True)
        elif kind == 'finished' and not message['success']:
            print('ERROR:', message['error'], file=sys.stderr)
    s=Settings(rom=Path(args.rom), project=root)
    return 0 if Pipeline(s, log).run() else 1

if __name__=='__main__':
    sys.exit(main())
