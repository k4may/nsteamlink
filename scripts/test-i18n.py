#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
spec = importlib.util.spec_from_file_location('compiler', Path(__file__).with_name('compile-i18n.py'))
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp)
    valid = json.dumps({'TITLE': '%d'})
    for catalog in ('en.json', 'pt-BR.json'):
        for invalid in ({'TITLE': '%s'}, {'TITLE': '%n'}, {'OTHER': '%d'}, {'TITLE': ''}):
            for name in ('zh-CN.json', 'en.json', 'pt-BR.json'):
                (source / name).write_text(valid)
            (source / catalog).write_text(json.dumps(invalid))
            try:
                compiler.compile_catalogs(source, source / 'out')
            except ValueError:
                pass
            else:
                raise AssertionError(f'invalid {catalog} translation accepted: {invalid}')
    for name in ('zh-CN.json', 'pt-BR.json'):
        (source / name).write_text(valid)
    (source / 'en.json').write_text('{"TITLE":"%d", "TITLE":"%d"}')
    try:
        compiler.compile_catalogs(source, source / 'out')
    except ValueError:
        pass
    else:
        raise AssertionError('duplicate resource accepted')
print('PASS catalog validation rejects missing keys and unsafe format changes')
