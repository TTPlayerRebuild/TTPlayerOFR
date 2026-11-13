"""Create an allowlisted, self-contained codec source attachment (no test data)."""
import argparse
from pathlib import Path
import re
import zipfile
from check_openfrog_source import verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--version', required=True)
    a = parser.parse_args(); root = a.root.resolve(); build = a.build.resolve()
    if not re.fullmatch(r'[0-9]{4}\.[0-9]{2}\.[0-9]{2}(?:p[1-9][0-9]*)?', a.version):
        raise ValueError('Invalid build version')
    plugin = 'ttp_ofr'
    verify(root / 'third_party/openfrog')
    destination = build / 'Release' / f'{plugin}-{a.version}-source.zip'
    with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        def tree(source, relative):
            for p in sorted(source.rglob('*')) if source.is_dir() else [source]:
                if p.is_symlink() or not p.is_file() or any(x in ('.git', '__pycache__') for x in p.parts):
                    continue
                if p.suffix.lower() in ('.obj', '.pdb', '.lib', '.exe', '.dll', '.pyc', '.log', '.o', '.a'):
                    continue
                z.write(p, plugin + '/' + (relative / p.relative_to(source) if source.is_dir() else relative).as_posix())
        for name in ('CMakeLists.txt', 'build.ps1', 'README.md', 'LICENSE', '.gitignore', '.clang-format', 'src', 'include', 'cmake', 'docs', '.github'):
            p = root / name
            if p.exists(): tree(p, Path(name))
        z.writestr(plugin + '/BUILD_VERSION', a.version + '\n')
        tree(root / 'third_party/openfrog', Path('third_party/openfrog'))
    print(destination)


if __name__ == '__main__': main()
