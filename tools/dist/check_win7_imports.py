"""Checks that Windows binaries run on Windows 7: every function they import must be one the Windows SDK declares for
Windows 7. The script lists the imports with dumpbin, then compiles a file that takes the address of each function
with _WIN32_WINNT / WINVER / NTDDI_VERSION set to Windows 7; the SDK hides functions of later Windows versions under
that setting, so those fail to compile. DLLs that need something not on a plain Windows 7 SP1 (the C++ runtime DLLs,
the Universal C runtime, DirectComposition, shcore) fail too, as does any DLL whose header this check doesn't include
(so a new dependency gets looked at).

Run in a Visual Studio x64 environment (dumpbin and cl on the PATH):
  python -I tools/dist/check_win7_imports.py <exe or dll> [...]
Exit code 0: all imports exist on Windows 7.
"""
import os
import re
import subprocess
import sys
import tempfile

# DLLs whose functions the test file declares (their headers below; winsock2.h must come before windows.h), all
# present on Windows 7 SP1.
CHECKED = {'kernel32.dll', 'user32.dll', 'gdi32.dll', 'shell32.dll', 'ole32.dll', 'oleaut32.dll', 'comdlg32.dll',
           'advapi32.dll', 'shlwapi.dll', 'winmm.dll', 'imm32.dll', 'dxgi.dll', 'version.dll', 'ws2_32.dll',
           'comctl32.dll', 'opengl32.dll', 'wininet.dll', 'crypt32.dll', 'setupapi.dll', 'dbghelp.dll'}
HEADERS = ['winsock2.h', 'windows.h', 'shellapi.h', 'shlobj.h', 'shlwapi.h', 'commdlg.h', 'mmsystem.h', 'imm.h',
           'dxgi.h', 'ole2.h', 'winreg.h', 'winver.h', 'commctrl.h', 'GL/gl.h', 'wininet.h', 'wincrypt.h', 'setupapi.h',
           'dbghelp.h']


def imports(path):
    out = subprocess.run(['dumpbin', '/nologo', '/imports', path], capture_output=True, text=True, check=True).stdout
    mods, cur = {}, None
    for line in out.splitlines():
        m = re.match(r'^\s{4}(\S+\.dll)\s*$', line, re.I)
        if m:
            cur = m.group(1).lower()
            mods.setdefault(cur, [])
            continue
        m = re.match(r'^\s+[0-9A-Fa-f]+\s+(\w+)\s*$', line)
        if m and cur:
            mods[cur].append(m.group(1))
    return mods


def main():
    funcs, problems = {}, []
    for path in sys.argv[1:]:
        for dll, names in imports(path).items():
            if dll not in CHECKED:
                problems.append(f'{os.path.basename(path)}: imports {dll}, which this check does not cover '
                                f'(not on Windows 7, or not reviewed yet)')
                continue
            for n in names:
                funcs.setdefault(n, dll)
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 'win7check.cpp')
        lines = ['#define _WIN32_WINNT 0x0601', '#define WINVER 0x0601', '#define NTDDI_VERSION 0x06010000',
                 '#define NOMINMAX'] + [f'#include <{h}>' for h in HEADERS] + ['']
        for i, (n, dll) in enumerate(sorted(funcs.items())):
            lines.append(f'void* f{i} = (void*)&::{n};  // {dll}')
        open(src, 'w').write('\n'.join(lines) + '\n')
        r = subprocess.run(['cl', '/nologo', '/c', '/EHsc', src, '/Fo' + os.path.join(d, 'win7check.obj')],
                           capture_output=True, text=True, cwd=d)
        missing = sorted(set(re.findall(
            r"'(\w+)': (?:is not a member of '`global namespace''|identifier not found|undeclared identifier)",
            r.stdout)))
        problems += [f'{n} ({funcs.get(n)}) is not declared for Windows 7' for n in missing]
        if r.returncode != 0 and not missing:
            problems.append('the check did not compile:\n' + r.stdout[-2000:])
    print(f'{len(funcs)} imported functions checked against the Windows 7 SDK declarations')
    for p in problems:
        print('  ' + p)
    print('OK: everything exists on Windows 7' if not problems else 'FAILED: not Windows 7 compatible')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
