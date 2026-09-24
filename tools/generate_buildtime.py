#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
"""Write the portable build timestamp used by the About dialog."""
import argparse
import datetime
import os
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--output', required=True)
args = parser.parse_args()
epoch = os.environ.get('SOURCE_DATE_EPOCH')
time = (datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc)
        if epoch else datetime.datetime.now().astimezone())
output = Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('#define CRFILES_BUILD_TIME "' + time.strftime('%m%d%H%M') + '"\n', encoding='utf-8')
