#!/usr/bin/env python3
"""Stage existing HEVC/HEIF DLLs and their dependencies; never build or download."""

import argparse
import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

SYSTEM_DLLS = {
    'kernel32.dll', 'kernelbase.dll', 'ntdll.dll', 'msvcrt.dll', 'ucrtbase.dll',
    'advapi32.dll', 'user32.dll', 'gdi32.dll', 'ole32.dll', 'oleaut32.dll',
    'shell32.dll', 'shlwapi.dll', 'ws2_32.dll', 'bcrypt.dll', 'crypt32.dll',
    'secur32.dll', 'version.dll', 'winmm.dll', 'normaliz.dll', 'rpcrt4.dll',
    'combase.dll', 'comdlg32.dll', 'imm32.dll', 'setupapi.dll', 'cfgmgr32.dll',
}
EXCLUDED_DLLS = {'dxcompiler.dll', 'dxil.dll'}


def check_pe(path):
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError(f'Not a Windows executable: {path}')
        stream.seek(struct.unpack_from('<I', header, 60)[0])
        if stream.read(6) != b'PE\0\0\x64\x86':
            raise ValueError(f'Expected Windows x64 DLL: {path}')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--runtime', default='', help='Defaults to OUT/hevc-runtime, OUT/heif-runtime, then OUT')
    parser.add_argument('--heif-runtime', default='', help='HEIF runtime directory; defaults to the shared runtime, OUT/heif-runtime, then OUT')
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    output = args.out_dir.resolve(strict=True)
    destination = args.destination.resolve(strict=True)
    project = Path(__file__).resolve().parent.parent
    if args.runtime:
        runtime = Path(args.runtime).resolve(strict=True)
    else:
        runtime = next((p for p in (output / 'hevc-runtime', output / 'heif-runtime') if p.exists()), output)
    if not runtime.is_dir():
        raise ValueError(f'Codec runtime must be a directory: {runtime}')

    def dlls(folder):
        result = {}
        for path in folder.iterdir():
            if path.is_file() and path.suffix.lower() == '.dll':
                key = path.name.lower()
                if key in result:
                    raise ValueError(f'Duplicate DLL name: {path}')
                result[key] = path
        return result

    if args.heif_runtime:
        heif_runtime = Path(args.heif_runtime).resolve(strict=True)
    elif 'heif.dll' in dlls(runtime):
        heif_runtime = runtime
    else:
        heif_runtime = output / 'heif-runtime' if (output / 'heif-runtime').exists() else output
    if not heif_runtime.is_dir():
        raise ValueError(f'HEIF runtime must be a directory: {heif_runtime}')

    groups = []
    for kind, folder, names, explicit, source, macro in (
        ('HEVC', runtime, ('libde265.dll', 'de265.dll'), bool(args.runtime),
         'crfiles_video_thumbnail.cc', 'CRFILES_DE265_FUNCTIONS'),
        ('HEIF', heif_runtime, ('heif.dll',), bool(args.heif_runtime),
         'crfiles_heic_thumbnail.cc', 'HEIC_SYMBOLS'),
    ):
        files = dlls(folder)
        decoder = next((files[name] for name in names if name in files), None)
        if decoder is None:
            if explicit or folder != output:
                raise ValueError(f'Missing {" or ".join(names)} in {kind} runtime: {folder}')
            setting = 'CRFILES_HEVC_RUNTIME_DIR' if kind == 'HEVC' else 'CRFILES_HEIF_RUNTIME_DIR'
            print(f'WARNING: No {kind} runtime found; {kind} thumbnails will be unavailable. '
                  f'Set {setting} to a prepared x64 runtime directory.', file=sys.stderr)
            continue
        groups.append((kind, folder, files, decoder, source, macro))
    if not groups:
        return

    reader = project.parent / 'third_party/llvm-build/Release+Asserts/bin/llvm-readobj'
    if not reader.is_file():
        located = shutil.which('llvm-readobj')
        if not located:
            raise ValueError('llvm-readobj is required to validate codec DLL dependencies')
        reader = Path(located)

    def inspect(path, option):
        check_pe(path)
        return subprocess.check_output([str(reader), option, str(path)], text=True)

    # Check the precise dynamically loaded API used by crFiles, without trying
    # to execute a Windows DLL on the Linux packaging host.
    selected = {}
    for kind, folder, files, decoder, source, macro in groups:
        declarations = (project / source).read_text()
        api = declarations.split(f'#define {macro}(V)', 1)[1].split('\n\n', 1)[0]
        required = set(re.findall(r'V\((\w+)\)', api))
        exported = set(re.findall(r'^\s+Name: (\S+)$', inspect(decoder, '--coff-exports'), re.MULTILINE))
        if not required or required - exported:
            raise ValueError(f'Incompatible {kind} decoder {decoder}: missing {sorted(required - exported)}')
        available = dlls(destination)
        available.update(dlls(output))
        available.update(files)
        pending = [decoder]
        while pending:
            path = pending.pop()
            name = path.name.lower()
            if name in selected:
                if digest(selected[name]) != digest(path):
                    raise ValueError(f'Conflicting codec DLL: {name}')
                continue
            if name in EXCLUDED_DLLS:
                raise ValueError(f'Codec requires excluded shader compiler: {name}')
            imports = inspect(path, '--coff-imports')
            selected[name] = path
            for dependency in re.findall(r'^\s+Name: (\S+\.dll)$', imports, re.MULTILINE | re.IGNORECASE):
                key = dependency.lower()
                if key in SYSTEM_DLLS or key.startswith(('api-ms-win-', 'ext-ms-win-')):
                    continue
                if key not in available:
                    raise ValueError(f'Missing dependency {dependency} required by {path.name}')
                pending.append(available[key])
    existing = dlls(destination)
    for name, path in selected.items():
        if name in existing and digest(existing[name]) != digest(path):
            raise ValueError(f'Conflicting packaged DLL: {name}')
    for name, path in sorted(selected.items()):
        # Canonical lowercase names also match the app's explicit loader paths.
        if name not in existing:
            shutil.copy2(path, destination / name)
    for kind, folder, *_ in groups:
        notices = folder / 'licenses'
        if notices.is_dir():
            shutil.copytree(notices, destination / f'licenses/{kind.lower()}-runtime', dirs_exist_ok=True)
    print(f'Codec DLLs packaged: {", ".join(sorted(selected))}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'Codec packaging failed: {error}')
