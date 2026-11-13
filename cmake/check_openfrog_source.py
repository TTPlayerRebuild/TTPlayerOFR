"""Verify the vendored source identity; no downloads or external source paths."""
import hashlib
import json
from pathlib import Path
import sys


def verify(root):
    identity = json.loads((root / 'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
    for relative, digest in identity['files'].items():
        path = root / relative
        if path.is_symlink() or root.resolve() not in path.resolve().parents:
            raise ValueError('Invalid snapshot member: ' + relative)
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError('OpenFROG source identity mismatch: ' + relative)
    print('OpenFROG source snapshot verified: ' + identity['commit'])
    return identity


if __name__ == '__main__':
    verify(Path(sys.argv[1]))
