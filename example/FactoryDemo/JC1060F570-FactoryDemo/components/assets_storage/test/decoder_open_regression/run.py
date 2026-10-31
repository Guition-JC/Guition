#!/usr/bin/env python3
## @file
# @brief Compile and run decoder dispatch and file-ownership checks against host stubs.
import argparse
import pathlib
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--expect-bug', action='store_true')
root = pathlib.Path(__file__).resolve().parents[4]
parser.add_argument('--source', type=pathlib.Path, default=root / 'managed_components/espressif__esp_lv_decoder/src/lvgl9/esp_lv_decoder.c')
parser.add_argument('--helper', action='append', default=[])
args = parser.parse_args()
source = args.source.read_text()

## @brief Extract a named static C function from the decoder source.
# @param name Function name to locate.
# @return Source text of the complete function definition.
def extract(name):
    pattern = re.compile(r'^static\s+[^;{}\n]+\b' + re.escape(name) + r'\s*\([^;]*?\)\s*\{', re.M)
    match = pattern.search(source)
    if not match:
        raise SystemExit('Cannot find function definition: ' + name)
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

directory = pathlib.Path(__file__).resolve().parent
stub = (directory / 'stubs.c').read_text()
tests = (directory / 'cases.c').read_text()
with tempfile.TemporaryDirectory(prefix='decoder_open_regression_') as output:
    unit = pathlib.Path(output) / 'decoder_open_test.c'
    unit.write_text(stub + '\n' + '\n'.join(extract(name) for name in args.helper + ['decoder_open']) + '\n' + tests)
    binary = pathlib.Path(output) / 'decoder_open_test'
    for pjpg in ([1] if args.expect_bug else [1, 0]):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-DESP_LV_ENABLE_PJPG=' + str(pjpg),
                        '-DEXPECT_BUG=' + str(int(args.expect_bug)),
                        str(unit), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
