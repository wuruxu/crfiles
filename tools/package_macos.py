#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Stage a standalone GN output as a signed macOS bundle.

Never builds, uploads, installs, or overwrites an existing output. Signing is
explicit; --unsigned is for inspecting layout only (not a sandbox test).
"""
import argparse
import datetime
import plistlib
import re
import shutil
import subprocess
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True).strip()


def load_plist(path):
    with path.open('rb') as stream:
        return plistlib.load(stream)


def write_plist(path, value):
    with path.open('wb') as stream:
        plistlib.dump(value, stream, sort_keys=False)


def version():
    values = dict(line.split('=', 1) for line in
                  (PROJECT / 'VERSION').read_text().splitlines() if '=' in line)
    return '.'.join(values[key] for key in ('MAJOR', 'MINOR', 'PATCH'))


def check_profile(path, bundle_id, team_id, distribution):
    profile = plistlib.loads(subprocess.check_output(
        ['security', 'cms', '-D', '-i', str(path)]))
    entitlements = profile.get('Entitlements', {})
    expected = f'{team_id}.{bundle_id}'
    if team_id not in profile.get('TeamIdentifier', []):
        raise ValueError('Provisioning profile belongs to a different team')
    if entitlements.get('com.apple.application-identifier') != expected:
        raise ValueError('Provisioning profile must match the explicit App ID')
    expiry = profile.get('ExpirationDate')
    if not expiry or expiry <= datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None):
        raise ValueError('Provisioning profile has expired')
    if distribution and (profile.get('ProvisionedDevices') or profile.get('ProvisionsAllDevices') or
                         entitlements.get('get-task-allow')):
        raise ValueError('A Mac App Store distribution profile is required for --pkg')
    return entitlements


def linked_libraries(binary):
    return [line.strip().split(' (', 1)[0]
            for line in run('otool', '-L', binary).splitlines()[1:]]


def check_minimum_system_version(binary, minimum):
    # Check every architecture, including older LC_VERSION_MIN_MACOSX binaries.
    commands = run('otool', '-l', binary)
    versions = re.findall(r'\bminos\s+(\d+(?:\.\d+){0,2})', commands)
    versions += re.findall(
        r'cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version (\d+(?:\.\d+){0,2})',
        commands)
    if not versions:
        raise ValueError(f'Cannot determine minimum macOS version: {binary}')
    def components(value):
        parts = tuple(int(part) for part in value.split('.'))
        return parts + (0,) * (3 - len(parts))
    for required in versions:
        if components(required) > components(minimum):
            raise ValueError(f'{binary} requires macOS {required}, but the bundle '
                             f'declares {minimum}; use a compatible build or raise '
                             '--minimum-system-version')


def uses_static_angle(build):
    flags = build / 'gen/ui/gl/buildflags.h'
    if not flags.is_file():
        raise ValueError(f'Missing generated GL build flags: {flags}')
    match = re.search(
        r'^#define BUILDFLAG_INTERNAL_USE_STATIC_ANGLE\(\) \(([01])\)$',
        flags.read_text(), re.MULTILINE)
    if not match:
        raise ValueError(f'Cannot determine static ANGLE configuration: {flags}')
    return match.group(1) == '1'


def copy_libraries(build, executables, destination, static_angle):
    # Dynamic ANGLE is loaded with dlopen and does not appear in otool -L.
    # Static builds generate placeholder dylibs for Chromium infrastructure;
    # they contain no ANGLE implementation and must not be bundled or signed.
    queue = list(executables)
    copied = set()
    if not static_angle:
        for name in ('libEGL.dylib', 'libGLESv2.dylib'):
            shutil.copy2(build / name, destination / name)
            copied.add(name)
            queue.append(destination / name)
    while queue:
        binary = queue.pop()
        for dependency in linked_libraries(binary):
            if dependency.startswith(('/System/Library/', '/usr/lib/')):
                continue
            name = Path(dependency).name
            # A dylib's first entry may be its own install name.
            if name == binary.name and binary.suffix == '.dylib':
                continue
            if not dependency.startswith(('@rpath/', '@loader_path/', '@executable_path/')):
                raise ValueError(f'Non-relocatable dependency: {dependency}')
            if not name.endswith('.dylib') or not (build / name).is_file():
                raise ValueError(f'Unsupported or missing dependency: {dependency}')
            if name not in copied:
                shutil.copy2(build / name, destination / name)
                copied.add(name)
                queue.append(destination / name)
            # All private dylibs are adjacent to both executables via symlinks.
            desired = f'@loader_path/{name}'
            if dependency != desired:
                run('install_name_tool', '-change', dependency, desired, binary)
    # Preserve LC_ID_DYLIB: consumers are relocated above, and ANGLE is opened
    # by bundle path. Growing the ID can exceed the linked Mach-O header space.
    return sorted(destination / name for name in copied)


def package(args):
    build = args.build_dir.resolve()
    output = args.output.resolve()
    if output.suffix != '.app' or output.exists():
        raise ValueError('--output must be a new .app path')
    if not re.fullmatch(r'[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', args.bundle_id):
        raise ValueError('Invalid bundle identifier')
    if not re.fullmatch(r'[A-Z0-9]{10}', args.team_id):
        raise ValueError('Invalid Apple Team ID')
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){1,2}', args.minimum_system_version):
        raise ValueError('Invalid minimum macOS version')
    if not re.fullmatch(r'[1-9][0-9]*(?:\.[0-9]+){0,2}', args.build_number):
        raise ValueError('Invalid CFBundleVersion')
    if args.developer_id and (args.profile or args.pkg or args.installer_identity):
        raise ValueError('--developer-id cannot use App Store profiles or installer signing')
    if not args.unsigned and not (args.identity and (args.profile or args.developer_id)):
        raise ValueError('Signing needs --identity and --profile; or explicitly use --unsigned')
    if args.unsigned and (args.identity or args.profile or args.pkg or args.installer_identity):
        raise ValueError('--unsigned cannot be combined with signing or installer arguments')
    if args.pkg and (not args.installer_identity or args.pkg.exists()):
        raise ValueError('--pkg requires --installer-identity and a new output path')
    static_angle = uses_static_angle(build)
    required = ['crFiles', 'icudtl.dat', 'crfiles_resources.pak', 'crfiles_resources']
    if not static_angle:
        required += ['libEGL.dylib', 'libGLESv2.dylib']
    for name in required:
        if not (build / name).exists():
            raise ValueError(f'Missing build resource: {name}')
    if not list(build.glob('crfiles_resources_*.pak')):
        raise ValueError('Missing localized resources')
    if not args.icon.is_file() or args.icon.read_bytes()[:4] != b'icns':
        raise ValueError('--icon must be an existing .icns file')
    check_minimum_system_version(build / 'crFiles', args.minimum_system_version)
    privacy_path = PROJECT / 'mac/PrivacyInfo.xcprivacy'
    privacy = load_plist(privacy_path)
    if not isinstance(privacy, dict) or not isinstance(
            privacy.get('NSPrivacyAccessedAPITypes'), list):
        raise ValueError('Missing or invalid privacy manifest')
    profile_entitlements = None
    if not args.unsigned and not args.developer_id:
        profile_entitlements = check_profile(args.profile, args.bundle_id, args.team_id, bool(args.pkg))
    contents = output / 'Contents'
    resources = contents / 'Resources'
    macos = contents / 'MacOS'
    helpers = contents / 'Helpers'
    frameworks = contents / 'Frameworks'
    for directory in (resources, macos, helpers, frameworks):
        directory.mkdir(parents=True, exist_ok=True)
    main = macos / 'crFiles'
    helper = helpers / 'crFiles Helper'
    shutil.copy2(build / 'crFiles', main)
    # Direct-download workers have the same signing requirements as the UI.
    # Preserve the existing worker launch path without duplicating executable
    # code. App Store workers still need a separate inheritance-only signature.
    executables = [main]
    if args.developer_id:
        helper.symlink_to(Path('../MacOS/crFiles'))
    else:
        shutil.copy2(build / 'crFiles', helper)
        executables.append(helper)
    for source in build.glob('crfiles_resources*.pak'):
        shutil.copy2(source, resources / source.name)
    shutil.copytree(build / 'crfiles_resources', resources / 'crfiles_resources')
    shutil.copy2(build / 'icudtl.dat', resources / 'icudtl.dat')
    shutil.copy2(args.icon, resources / 'crFiles.icns')
    shutil.copy2(privacy_path, resources / 'PrivacyInfo.xcprivacy')
    for localization in sorted((PROJECT / 'mac').glob('*.lproj')):
        shutil.copytree(localization, resources / localization.name)
    libraries = copy_libraries(build, executables, frameworks, static_angle)
    for library in libraries:
        check_minimum_system_version(library, args.minimum_system_version)
        for directory in (macos, helpers):
            (directory / library.name).symlink_to(Path('../Frameworks') / library.name)
    if args.strip:
        # Only modify staged copies, before signing. Preserve external symbols
        # for dynamic linking and dlsym while removing debug and local symbols.
        for binary in [*executables, *libraries]:
            before = binary.stat().st_size
            run('xcrun', 'strip', '-S', '-x', binary)
            after = binary.stat().st_size
            print(f'Stripped {binary.name}: {before} -> {after} bytes')
    info = load_plist(PROJECT / 'mac/Info.plist')
    info.update(CrFilesIPCGroup=f'{args.team_id}.{args.bundle_id}.ipc',
                CFBundleIdentifier=args.bundle_id,
                CFBundleShortVersionString=version(), CFBundleVersion=args.build_number,
                LSMinimumSystemVersion=args.minimum_system_version)
    write_plist(contents / 'Info.plist', info)
    (contents / 'PkgInfo').write_bytes(b'APPL????')
    if args.unsigned:
        print(f'Staged unsigned layout: {output}; not suitable for sandbox validation or upload')
        return
    if args.profile:
        shutil.copy2(args.profile, contents / 'embedded.provisionprofile')
    # Keep generated entitlements next to the output for inspection, outside
    # the sealed bundle. Sign nested code before the enclosing app, never --deep.
    entitlement_path = output.with_suffix('.entitlements')
    if entitlement_path.exists():
        raise ValueError(f'Refusing to overwrite {entitlement_path}')
    signing_options = ['--options', 'runtime'] if args.developer_id else []
    main_options = []
    helper_options = []
    if not args.developer_id:
        entitlements = load_plist(PROJECT / 'mac/App.entitlements')
        entitlements['com.apple.security.application-groups'] = [f'{args.team_id}.{args.bundle_id}.ipc']
        entitlements['com.apple.application-identifier'] = f'{args.team_id}.{args.bundle_id}'
        entitlements['com.apple.developer.team-identifier'] = args.team_id
        if profile_entitlements.get('get-task-allow'):
            entitlements['get-task-allow'] = True
        write_plist(entitlement_path, entitlements)
        main_options = ['--entitlements', entitlement_path]
        helper_options = ['--entitlements', PROJECT / 'mac/Helper.entitlements']
    for library in libraries:
        run('codesign', '--force', '--sign', args.identity, '--timestamp',
            *signing_options, library)
    if not args.developer_id:
        run('codesign', '--force', '--sign', args.identity, '--timestamp',
            *signing_options, '--identifier', f'{args.bundle_id}.helper',
            *helper_options, helper)
    run('codesign', '--force', '--sign', args.identity, '--timestamp',
        *signing_options, *main_options, output)
    run('codesign', '--verify', '--deep', '--strict', '--verbose=2', output)
    details = subprocess.run(['codesign', '-dv', '--verbose=4', str(output)],
                             capture_output=True, text=True, check=True).stderr
    if f'TeamIdentifier={args.team_id}' not in details.splitlines():
        raise ValueError('Signing identity Team ID does not match --team-id')
    if args.developer_id and not any(
            line.startswith('Authority=Developer ID Application:')
            for line in details.splitlines()):
        raise ValueError('--developer-id requires a Developer ID Application certificate')
    if args.pkg:
        run('productbuild', '--component', output, '/Applications', '--sign',
            args.installer_identity, args.pkg)
    print(f'Signed bundle: {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--icon', required=True, type=Path)
    parser.add_argument('--bundle-id', default='com.libcr.crfiles')
    parser.add_argument('--team-id', default='6JYK2B5HZ3')
    parser.add_argument('--build-number', required=True)
    parser.add_argument('--minimum-system-version', default='13.0')
    parser.add_argument('--identity')
    parser.add_argument('--profile', type=Path)
    parser.add_argument('--installer-identity')
    parser.add_argument('--pkg', type=Path)
    parser.add_argument('--unsigned', action='store_true')
    parser.add_argument('--strip', action='store_true',
                        help='Strip debug and local symbols from staged binaries before signing')
    parser.add_argument('--developer-id', action='store_true',
                        help='Sign a non-sandboxed direct-download app with hardened runtime')
    args = parser.parse_args()
    try:
        package(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Packaging failed: {error}\n')


if __name__ == '__main__':
    main()
