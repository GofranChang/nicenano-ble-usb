#!/usr/bin/env python3
"""Build using an existing Zephyr 3.5/ZMK v0.3 dependency workspace; never flash."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

app = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--workspace', type=Path,
                    default=app.parent / 'zmk-local')
parser.add_argument('--diagnostic', action='store_true', help='USB CDC diagnostics and internal LF clock')
parser.add_argument('--xtal', action='store_true', help='Diagnostic LF crystal clock comparison')
args = parser.parse_args()
if args.xtal and not args.diagnostic:
    parser.error('--xtal requires --diagnostic')
root = args.workspace.resolve()
env = os.environ.copy()
env['PATH'] = str(root / '.venv/bin') + ':/opt/homebrew/bin:' + env['PATH']
env['ZEPHYR_BASE'] = str(root / 'zephyr')
env['Zephyr_DIR'] = str(root / 'zephyr/share/zephyr-package/cmake')
env['ZEPHYR_TOOLCHAIN_VARIANT'] = 'zephyr'
env.setdefault('ZEPHYR_SDK_INSTALL_DIR', str(root / 'zephyr-sdk-0.16.8'))
env['PYTHONDONTWRITEBYTECODE'] = '1'
modules = [root / 'modules' / p for p in (
    'hal/cmsis', 'hal/nordic', 'crypto/tinycrypt', 'debug/segger', 'lib/picolibc')]
for p in [root / '.venv/bin/west', root / 'zephyr', *modules]:
    if not p.exists():
        parser.error('Missing dependency: ' + str(p))
build_dir=app / ('build-diagnostic' if args.diagnostic else 'build')
log_file=app / ('build-diagnostic.log' if args.diagnostic else 'build.log')
cmd = [str(root / '.venv/bin/west'), 'build', '-p', 'always', str(app),
       '-d', str(build_dir), '-b', 'nice_nano_v2_bridge', '--',
       '-DZEPHYR_MODULES=' + ';'.join(map(str, modules)),
       '-DUSER_CACHE_DIR=' + str(app / 'build-cache')]
if args.diagnostic:
    extra=str(app/'diagnostic.conf')
    if args.xtal: extra+=';'+str(app/'clock-xtal.conf')
    cmd += ['-DEXTRA_CONF_FILE='+extra,
            '-DDTC_OVERLAY_FILE='+str(app/'diagnostic.overlay')]
with log_file.open('w') as log:
    process = subprocess.Popen(cmd, cwd=root, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    for line in process.stdout:
        log.write(line)
    code = process.wait()
if code:
    print(log_file.read_text()[-6000:])
    sys.exit(code)
dest = app / 'firmware'
dest.mkdir(exist_ok=True)
stem='nicenano-ble-usb-diagnostic-v8.2-web' if args.diagnostic else 'nicenano-v2-ble-usb-wenwan'
if args.xtal: stem+='-xtal'
shutil.copy2(build_dir / 'zephyr/zephyr.uf2', dest / (stem+'.uf2'))
shutil.copy2(build_dir / 'zephyr/zephyr.elf', dest / (stem+'.elf'))
shutil.copy2(build_dir / 'zephyr/.config', dest / (stem+'.config'))
print(log_file.read_text()[-600:])
print('UF2:', dest / (stem+'.uf2'))
