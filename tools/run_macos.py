#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Package existing crFiles artifacts and launch the development .app in front.

Never compiles or installs. Replaces only a development bundle produced by this
toolchain, after staging and signature verification. Quit crFiles before running.
"""
import argparse
import fcntl
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
import package_macos


def require_closed():
    result = subprocess.run(['/usr/bin/pgrep', '-x', 'crFiles'],
                            capture_output=True, text=True)
    if result.returncode == 0:
        raise ValueError('Quit the existing crFiles instance before packaging or launching the app.')
    if result.returncode != 1:
        raise ValueError('Cannot check whether crFiles is running: ' + result.stderr.strip())


def check_output(output, bundle_id):
    if output.suffix != '.app' or output.is_symlink():
        raise ValueError('--output must be a .app directory, not a symlink')
    if not output.exists():
        return
    info_path = output / 'Contents/Info.plist'
    if not info_path.is_file():
        raise ValueError(f'Refusing to replace an unrelated output: {output}')
    info = package_macos.load_plist(info_path)
    if (info.get('CrFilesDevelopmentBundle') is not True or
            info.get('CFBundleIdentifier') != bundle_id):
        raise ValueError(f'Refusing to replace a non-development application: {output}')


def prepare(args, output):
    check_output(output, args.bundle_id)
    # Stage on the same filesystem; keep the old bundle until the new one is
    # completely assembled and verified. Never modify the input executable.
    with tempfile.TemporaryDirectory(prefix='.crfiles-app-', dir=output.parent) as temporary:
        staging = Path(temporary)
        app = staging / 'crFiles.app'
        package_macos.package(argparse.Namespace(
            build_dir=args.build_dir, output=app, icon=package_macos.PROJECT / 'crFiles.icns',
            bundle_id=args.bundle_id, team_id=args.team_id, build_number='1',
            minimum_system_version=args.minimum_system_version,
            identity=args.identity, profile=None, installer_identity=None, pkg=None,
            unsigned=False, strip=False, developer_id=False, development=True))
        check_output(output, args.bundle_id)
        previous = staging / 'previous.app'
        if output.exists():
            output.rename(previous)
        try:
            app.rename(output)
        except OSError:
            if previous.exists():
                previous.rename(output)
            raise
    print(f'Development app: {output}', flush=True)


def launch(output, app_args):
    require_closed()
    logs = Path(tempfile.mkdtemp(prefix='crfiles-app-'))
    app_log = logs / 'app.log'
    stdout_log = logs / 'stdout.log'
    app_log.touch()
    stdout_log.touch()
    # Launch Services opens the exact bundle in the foreground. Passing the
    # bundle executable directly would retain the old Terminal launch path.
    command = ['/usr/bin/open', '-n', '-W', '-a', str(output),
               '--stdout', str(stdout_log), '--stderr', str(app_log)]
    if app_args:
        command += ['--args', *app_args]
    print(f'Application log: {app_log}\nStandard output: {stdout_log}', flush=True)
    print('Open Quick Share discovery in the foreground and respond to the location prompt. '
          'Quit crFiles to finish; Ctrl+C only stops waiting for it.', flush=True)
    with subprocess.Popen(command) as process, app_log.open(encoding='utf-8', errors='replace') as log:
        try:
            while process.poll() is None:
                chunk = log.read(65536)
                if chunk:
                    print(chunk, end='', flush=True)
                else:
                    time.sleep(0.1)
            print(log.read(), end='', flush=True)
            if process.returncode:
                raise ValueError(f'Launch Services returned {process.returncode}; see {app_log}')
        except KeyboardInterrupt:
            process.terminate()
            process.wait()
            print(f'\nStopped waiting. Quit crFiles from its window. Log: {app_log}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        help='Development bundle path; defaults to BUILD_DIR/crFiles.app.')
    parser.add_argument('--identity', default='-',
                        help='Local signing identity; defaults to ad-hoc signing without a certificate.')
    parser.add_argument('--bundle-id', default='com.libcr.crfiles')
    parser.add_argument('--team-id', default='6JYK2B5HZ3')
    parser.add_argument('--minimum-system-version', default='13.0')
    parser.add_argument('--prepare-only', action='store_true',
                        help='Package and verify without opening any application.')
    parser.add_argument('build_dir', type=Path, help='Existing build directory, for example out/arm64.')
    parser.add_argument('app_args', nargs=argparse.REMAINDER, help='Arguments passed to crFiles.')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('This launcher requires macOS.')
    args.build_dir = args.build_dir.expanduser().resolve()
    requested = args.output or args.build_dir / 'crFiles.app'
    requested = requested.expanduser().absolute()
    output = requested.parent.resolve() / requested.name
    app_args = args.app_args[1:] if args.app_args[:1] == ['--'] else args.app_args
    try:
        if args.prepare_only and app_args:
            raise ValueError('--prepare-only does not accept application arguments')
        if not args.build_dir.is_dir():
            raise ValueError(f'Build directory does not exist: {args.build_dir}')
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.with_suffix('.run.lock').open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('Another development launcher is using this output.') from None
            if output.exists() or not args.prepare_only:
                require_closed()
            prepare(args, output)
            if not args.prepare_only:
                launch(output, app_args)
    except (ValueError, OSError, plistlib.InvalidFileException,
            subprocess.CalledProcessError) as error:
        parser.exit(1, f'Development launch failed: {error}\n')


if __name__ == '__main__':
    main()
