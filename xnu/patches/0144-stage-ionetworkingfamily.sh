#!/bin/bash
# Stage Apple OSS IONetworkingFamily-170.140.2 into the XNU tree for SWEET/topaz.
# dxb runs patches/*.sh from the XNU source root before applying *.patch.
# Paired with 0144-topaz-ionetworking-family.patch (files.arm64 + config + IONetworkStack).
set -euo pipefail

[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
[[ "${MACOS_VERSION:-}" == 13.5 ]] || {
	echo 'SWEET IONetworkingFamily integration requires xnu-8796 (macOS 13.5)' >&2
	exit 1
}
[[ -f iokit/KernelConfigTables.cpp ]] || exit 1

# Tag IONetworkingFamily-170.140.2 (macOS 13.5 / xnu-8796.141 era).
ionet_sha=f9f7e57b2132b82de09a26ddcbf56fd8125b4933
ionet_tmp=$(mktemp -d)
trap 'rm -rf "$ionet_tmp"' EXIT
git init -q "$ionet_tmp"
git -C "$ionet_tmp" fetch -q --depth 1 \
	https://github.com/apple-oss-distributions/IONetworkingFamily "$ionet_sha"
git -C "$ionet_tmp" checkout -q FETCH_HEAD
[[ "$(git -C "$ionet_tmp" rev-parse HEAD)" == "$ionet_sha" ]]

python3 - "$ionet_tmp" <<'PY'
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
family = Path('iokit/Families/IONetworkingFamily')
pub = Path('iokit/IOKit/network')
if family.exists():
    raise SystemExit(f'refusing to overwrite existing IONetworkingFamily: {family}')
family.mkdir(parents=True)
pub.mkdir(parents=True)

# Public headers installed as <IOKit/network/*.h> (kext SDK layout).
public_headers = [
    'IOBasicOutputQueue.h',
    'IOEthernetController.h',
    'IOEthernetInterface.h',
    'IOEthernetStats.h',
    'IOGatedOutputQueue.h',
    'IOKernelDebugger.h',
    'IOMbufMemoryCursor.h',
    'IONetworkController.h',
    'IONetworkData.h',
    'IONetworkInterface.h',
    'IONetworkMedium.h',
    'IONetworkStats.h',
    'IONetworkUserClient.h',
    'IOOutputQueue.h',
    'IOPacketQueue.h',
]
# Private / build-local headers stay next to the .cpp units.
private_headers = [
    'IOMbufQueue.h',
    'IONetworkControllerPrivate.h',
    'IONetworkDebug.h',
    'IONetworkStack.h',
    'IONetworkTypesPrivate.h',
]
sources = [
    'IOEthernetController.cpp',
    'IOEthernetInterface.cpp',
    'IOKernelDebugger.cpp',
    'IOMbufMemoryCursor.cpp',
    'IONetworkController.cpp',
    'IONetworkData.cpp',
    'IONetworkInterface.cpp',
    'IONetworkMedium.cpp',
    'IONetworkStack.cpp',
    'IONetworkUserClient.cpp',
    'IOOutputQueue.cpp',
    'IOPacketQueue.cpp',
]

for name in public_headers:
    shutil.copy2(source / name, pub / name)
for name in private_headers + sources:
    shutil.copy2(source / name, family / name)
shutil.copy2(source / 'APPLE_LICENSE', family / 'APPLE_LICENSE')

# Minimal AssertMacros for require() used by IOEthernetController AVB path.
(family / 'AssertMacros.h').write_text(
    '/* SWEET: minimal AssertMacros for in-kernel IONetworkingFamily */\n'
    '#ifndef __AssertMacros_h__\n'
    '#define __AssertMacros_h__\n'
    '#define require(assertion, exceptionLabel) \\\n'
    '    do { if (__builtin_expect(!(assertion), 0)) goto exceptionLabel; } while (0)\n'
    '#endif /* __AssertMacros_h__ */\n'
)

# Kext build used bare <IOEthernet*.h>; point at IOKit/network instead.
eth_if = family / 'IOEthernetInterface.cpp'
text = eth_if.read_text()
text = text.replace('#include <IOEthernetInterface.h>',
                    '#include <IOKit/network/IOEthernetInterface.h>')
text = text.replace('#include <IOEthernetController.h>',
                    '#include <IOKit/network/IOEthernetController.h>')
eth_if.write_text(text)

ctrl = family / 'IOEthernetController.cpp'
ct = ctrl.read_text()
ct = ct.replace('#include <AssertMacros.h>', '#include "AssertMacros.h"')
ctrl.write_text(ct)

# Prefer sibling TargetConditionals.h (copied from SDK by the shell below).
for cpp in family.glob('*.cpp'):
    t = cpp.read_text()
    n = t.replace('#include <TargetConditionals.h>',
                  '#include "TargetConditionals.h"')
    if n != t:
        cpp.write_text(n)
PY

# Kernel builds use -nostdinc; some units include TargetConditionals.h.
sdk=$(xcrun --sdk macosx --show-sdk-path)
cp "$sdk/usr/include/TargetConditionals.h" iokit/Families/IONetworkingFamily/
echo "Staged IONetworkingFamily-170.140.2 ($ionet_sha) for SWEET"
