"""Verify the vendored source identity; no downloads or external source paths."""
import hashlib
import json
from pathlib import Path
import re
import sys


def snapshot_digest(files):
    data = ''.join(digest + '  ' + name + '\n' for name, digest in sorted(files.items()))
    return hashlib.sha256(data.encode('utf-8')).hexdigest()


def verify(root):
    identity = json.loads((root / 'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
    if (identity.get('schema_version') != 2 or not re.fullmatch(r'[0-9a-f]{40}', identity['commit'])
            or identity.get('upstream_worktree') != 'clean'):
        raise ValueError('Invalid OpenFROG snapshot identity')
    actual = {p.relative_to(root).as_posix() for folder in ('src', 'include')
              for p in (root / folder).rglob('*') if p.is_file()}
    if actual | {'LICENSE'} != set(identity['files']):
        raise ValueError('OpenFROG snapshot inventory mismatch')
    for relative, digest in identity['files'].items():
        path = root / relative
        if ('\\' in relative or '..' in Path(relative).parts or path.is_symlink()
                or root.resolve() not in path.resolve().parents
                or not re.fullmatch(r'[0-9a-f]{64}', digest)):
            raise ValueError('Invalid snapshot member: ' + relative)
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError('OpenFROG source identity mismatch: ' + relative)
    if snapshot_digest(identity['files']) != identity['snapshot_sha256']:
        raise ValueError('OpenFROG snapshot digest mismatch')
    print('OpenFROG source snapshot verified: ' + identity['commit'])
    return identity


if __name__ == '__main__':
    verify(Path(sys.argv[1]))
