#!/usr/bin/env python3
"""Stage existing HEVC DLLs and their dependencies; never build or download."""

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

    runtime_files = dlls(runtime)
    decoder = next((runtime_files[name] for name in ('libde265.dll', 'de265.dll') if name in runtime_files), None)
    if decoder is None:
        if args.runtime or runtime != output:
            raise ValueError(f'Missing libde265.dll or de265.dll in codec runtime: {runtime}')
        print('WARNING: No HEVC runtime found; HEVC thumbnails will be unavailable. '
              'Set CRFILES_HEVC_RUNTIME_DIR to a prepared x64 runtime directory.', file=sys.stderr)
        return

    reader = project.parent / 'third_party/llvm-build/Release+Asserts/bin/llvm-readobj'
    if not reader.is_file():
        located = shutil.which('llvm-readobj')
        if not located:
            raise ValueError('llvm-readobj is required to validate HEVC DLL dependencies')
        reader = Path(located)

    def inspect(path, option):
        check_pe(path)
        return subprocess.check_output([str(reader), option, str(path)], text=True)

    # Check the precise dynamically loaded API used by crFiles, without trying
    # to execute a Windows DLL on the Linux packaging host.
    source = (project / 'crfiles_video_thumbnail.cc').read_text()
    api = source.split('#define CRFILES_DE265_FUNCTIONS(V)', 1)[1].split('class HevcDecoder', 1)[0]
    required = set(re.findall(r'V\((de265_\w+)\)', api))
    exported = set(re.findall(r'^\s+Name: (\S+)$', inspect(decoder, '--coff-exports'), re.MULTILINE))
    if not required or required - exported:
        raise ValueError(f'Incompatible HEVC decoder {decoder}: missing {sorted(required - exported)}')
    available = dlls(destination)
    available.update(dlls(output))
    available.update(runtime_files)
    selected = {}
    pending = [decoder]
    while pending:
        path = pending.pop()
        name = path.name.lower()
        if name in selected:
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
    notices = runtime / 'licenses'
    if notices.is_dir():
        shutil.copytree(notices, destination / 'licenses/hevc-runtime', dirs_exist_ok=True)
    print(f'HEVC runtime: {runtime}; packaged {", ".join(sorted(selected))}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'HEVC packaging failed: {error}')
