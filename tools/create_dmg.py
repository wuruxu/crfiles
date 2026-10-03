#!/usr/bin/env python3
"""Create a Finder-configured disk image from an already signed app stage."""
import argparse
import plistlib
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


def run(*args):
    return subprocess.check_output([str(arg) for arg in args])


def wait_for_layout(path, timeout=30):
    # Finder writes .DS_Store asynchronously, including after the window closes.
    # Do not detach while the file is missing, empty, or still changing.
    deadline = time.monotonic() + timeout
    previous = None
    stable_since = None
    while time.monotonic() < deadline:
        try:
            stat = path.stat()
            with path.open('rb') as stream:
                valid = stream.read(8) == b'\x00\x00\x00\x01Bud1'
            current = (stat.st_size, stat.st_mtime_ns) if valid else None
        except FileNotFoundError:
            current = None
        now = time.monotonic()
        if current is not None and current == previous:
            if stable_since is not None and now - stable_since >= 3:
                return True
        else:
            stable_since = now if current is not None else None
        previous = current
        time.sleep(0.5)
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--app-name', default='crFiles')
    args = parser.parse_args()
    stage = args.stage.resolve()
    output = args.output.absolute()
    if output.exists():
        parser.error('Output already exists')
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]*', args.app_name):
        parser.error('Invalid app name')
    app_filename = f'{args.app_name}.app'
    if not (stage / app_filename).is_dir():
        parser.error(f'Missing staged {app_filename}')
    background = Path(__file__).resolve().parents[1] / 'mac/dmg/background.png'
    (stage / '.background').mkdir(exist_ok=True)
    shutil.copy2(background, stage / '.background/background.png')
    with tempfile.TemporaryDirectory(prefix=f'{args.app_name.lower()}-dmg-') as temp:
        temp = Path(temp)
        image = temp / 'layout.dmg'
        # Finder can resolve aliases to an already mounted volume with the same
        # name. Use a unique staging volume, then rename it after saving layout.
        run('hdiutil', 'create', '-volname', temp.name,
            '-srcfolder', stage, '-format', 'UDRW', image)
        result = plistlib.loads(run('hdiutil', 'attach', '-readwrite', '-nobrowse',
                                    '-plist', image))
        volume = next(item for item in result['system-entities']
                      if 'mount-point' in item)
        mount = Path(volume['mount-point'])
        device = volume['dev-entry']
        try:
            # Pass the mount path as argv, not interpolated AppleScript source.
            script = '''on run argv
    tell application "Finder"
        set targetFolder to POSIX file (item 1 of argv) as alias
        open targetFolder
        set targetWindow to container window of targetFolder
        set current view of targetWindow to icon view
        set toolbar visible of targetWindow to false
        set statusbar visible of targetWindow to false
        set bounds of targetWindow to {200, 160, 920, 600}
        set options to icon view options of targetWindow
        set arrangement of options to not arranged
        set icon size of options to 90
        set text size of options to 13
        set background picture of options to file ".background:background.png" of targetFolder
        set position of item (item 2 of argv) of targetFolder to {210, 220}
        set position of item "Applications" of targetFolder to {510, 220}
        update targetFolder without registering applications
        delay 2
        close targetWindow
    end tell
end run
'''
            for attempt in range(2):
                subprocess.run(['osascript', '-', str(mount), app_filename], input=script,
                               text=True, check=True, timeout=45)
                print('Waiting for Finder to save the disk image layout...', flush=True)
                if wait_for_layout(mount / '.DS_Store'):
                    break
                if attempt == 0:
                    print('Finder has not saved the layout; reopening and retrying...', flush=True)
            else:
                raise RuntimeError(
                    'Finder did not save .DS_Store after two attempts. Check Finder '
                    'automation permission and desktopservices DSDontWriteUSBStores/'
                    'DSDontWriteNetworkStores preferences. No final DMG was created.')
            run('diskutil', 'renameVolume', device, f'{args.app_name} {args.version}')
        finally:
            run('hdiutil', 'detach', device)
        run('hdiutil', 'convert', image, '-format', 'UDZO', '-o', output)
    print(f'Created disk image: {output}')


if __name__ == '__main__':
    main()
