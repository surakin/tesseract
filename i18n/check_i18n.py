#!/usr/bin/env python3
"""i18n consistency checks, run by ctest (tests/CMakeLists.txt).

1. Every string the UI sources mark for translation (tk::tr / trn / trf,
   tk::N_, macOS TkTr) has an entry in every i18n/*.po catalog. A missing
   entry silently falls back to English at runtime.
2. The native shells don't call a translator other than tk::tr: a bare tr()
   inside a QObject is QObject::tr (no QTranslator is installed), and GTK's
   _() is libc gettext (no text domain is bound). Both always return English.

Usage: check_i18n.py <source-root>
Needs xgettext on PATH. Exits 1 and lists the problems when a check fails.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

KEYWORDS = ['tr:1', 'trn:1,2', 'trf:1', 'N_:1', 'TkTr:1']
SOURCE_SUFFIXES = {'.cpp', '.h', '.hpp', '.mm'}
SHELL_DIRS = ['ui/linux-qt', 'ui/linux-gtk', 'ui/windows', 'ui/macos']


def ui_sources(root):
    for path in sorted((root / 'ui').rglob('*')):
        if path.suffix in SOURCE_SUFFIXES and 'third_party' not in path.parts:
            yield path


def unquote(s):
    return (s.replace('\\n', '\n').replace('\\t', '\t').replace('\\"', '"')
             .replace('\\\\', '\\'))


def po_msgids(text):
    """msgids of a .po/.pot, excluding the header and obsolete entries."""
    ids = set()
    cur = None
    for line in text.splitlines():
        if line.startswith('#~'):
            continue
        m = re.match(r'^msgid "(.*)"$', line)
        if m:
            cur = [m.group(1)]
            continue
        m = re.match(r'^"(.*)"$', line)
        if m and cur is not None:
            cur.append(m.group(1))
            continue
        if cur is not None:
            msgid = unquote(''.join(cur))
            if msgid:
                ids.add(msgid)
            cur = None
    if cur is not None and ''.join(cur):
        ids.add(unquote(''.join(cur)))
    return ids


def extract(root, files):
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / 'src.pot'
        lst = pathlib.Path(tmp) / 'files.txt'
        lst.write_text('\n'.join(str(f.relative_to(root)) for f in files))
        cmd = ['xgettext', '--language=C++', '--from-code=UTF-8',
               '--no-wrap', '-o', str(out), '-f', str(lst)]
        cmd += ['--keyword=' + k for k in KEYWORDS]
        subprocess.run(cmd, cwd=root, check=True, stderr=subprocess.DEVNULL)
        return po_msgids(out.read_text(encoding='utf-8')) if out.exists() else set()


def strip_comments(src):
    # Blank out comments and string/char literals, keeping line numbers.
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                         re.S)
    return pattern.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), src)


BARE_TR = re.compile(r'(?<![\w:.>])(tr|_)\s*\(')


def bare_translator_calls(root):
    problems = []
    for d in SHELL_DIRS:
        for path in sorted((root / d).rglob('*')):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            code = strip_comments(path.read_text(encoding='utf-8', errors='replace'))
            for m in BARE_TR.finditer(code):
                line = code.count('\n', 0, m.start()) + 1
                problems.append(f'{path.relative_to(root)}:{line}: bare {m.group(1)}() '
                                f'-- use tk::tr()')
    return problems


def main():
    root = pathlib.Path(sys.argv[1]).resolve()
    failed = False

    source_ids = extract(root, list(ui_sources(root)))
    for po in sorted((root / 'i18n').glob('*.po')):
        missing = sorted(source_ids - po_msgids(po.read_text(encoding='utf-8')))
        if missing:
            failed = True
            print(f'{po.relative_to(root)}: {len(missing)} string(s) missing:')
            for msgid in missing:
                print('    ' + repr(msgid))

    bare = bare_translator_calls(root)
    if bare:
        failed = True
        print('\n'.join(bare))

    if not failed:
        print(f'OK: {len(source_ids)} strings, all catalogs complete')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
