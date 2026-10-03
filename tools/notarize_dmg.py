#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Submit an existing signed DMG, require acceptance, and staple its ticket."""
import argparse
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dmg', required=True, type=Path)
    parser.add_argument('--key', type=Path)
    parser.add_argument('--key-id')
    parser.add_argument('--issuer', default='')
    parser.add_argument('--keychain-profile', default='')
    args = parser.parse_args()
    if not args.dmg.is_file():
        parser.error('DMG not found; create the signed disk image before notarization')
    if args.keychain_profile:
        auth = ['--keychain-profile', args.keychain_profile]
    else:
        if not args.key or not args.key.is_file() or not args.key_id:
            parser.error('Provide an existing API key and key ID, or a keychain profile')
        auth = ['--key', str(args.key), '--key-id', args.key_id]
        # Individual keys must omit issuer. Team keys require their real UUID.
        if args.issuer:
            auth += ['--issuer', args.issuer]
    result_path = args.dmg.with_suffix('.notary.json')
    log_path = args.dmg.with_suffix('.notary-log.json')
    print('Submitting DMG to Apple; waiting for notarization...', flush=True)
    result = subprocess.run(
        ['xcrun', 'notarytool', 'submit', str(args.dmg), *auth,
         '--wait', '--output-format', 'json'], capture_output=True, text=True)
    result_path.write_text(result.stdout)
    if result.stderr:
        print(result.stderr, end='')
    try:
        submission = json.loads(result.stdout)
    except json.JSONDecodeError:
        raise SystemExit(f'Notarization did not return JSON; see {result_path}')
    print(f"Submission: {submission.get('id', 'unknown')}; "
          f"status: {submission.get('status', 'unknown')}", flush=True)
    if result.returncode or submission.get('status') != 'Accepted':
        if submission.get('id'):
            subprocess.run(['xcrun', 'notarytool', 'log', submission['id'],
                            *auth, str(log_path)], check=False)
        raise SystemExit(f'Notarization failed; see {result_path} and {log_path}')
    subprocess.run(['xcrun', 'stapler', 'staple', str(args.dmg)], check=True)
    subprocess.run(['xcrun', 'stapler', 'validate', str(args.dmg)], check=True)
    subprocess.run(['spctl', '--assess', '--type', 'open', '--context',
                    'context:primary-signature', '--verbose=2', str(args.dmg)],
                   check=True)
    print(f'Notarized DMG ready: {args.dmg}')


if __name__ == '__main__':
    main()
