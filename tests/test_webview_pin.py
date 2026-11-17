"""Release regression: host Edge updates must not silently change our runtime."""
import hashlib
import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('pinned_runtime', ROOT / 'tools/pinned_webview_runtime.py')
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class PinTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / 'installed-157.0.4341.46'
        self.source.mkdir()
        self.pin = self.root / 'pin.json'
        self.data = {'schema': 1, 'runtimeVersion': '157.0.4341.46', 'baselineRelease': '3.3.18', 'files': []}
        for name, content in [('msedgewebview2.exe', b'pinned-runtime'), ('Locales/en-US.pak', b'english'), ('Locales/zh-CN.pak', b'chinese')]:
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            self.data['files'].append({'path': name, 'bytes': len(content), 'sha256': hashlib.sha256(content).hexdigest()})
        self.save()

    def save(self):
        self.pin.write_text(json.dumps(self.data), encoding='utf-8')

    def test_identical_bytes_and_only_pinned_files_are_bundled(self):
        (self.source / 'new-host-only.dat').write_bytes(b'host update extras')
        output = self.root / 'output'
        proof = runtime.copy_pinned_runtime(self.pin, [self.source], output)
        self.assertEqual(proof['files'], 3)
        self.assertEqual(sorted(p.relative_to(output).as_posix() for p in output.rglob('*') if p.is_file()), sorted(x['path'] for x in self.data['files']))
        for entry in self.data['files']:
            self.assertEqual((output / entry['path']).read_bytes(), (self.source / entry['path']).read_bytes())

    def test_host_patch_update_is_rejected_before_staging(self):
        (self.source / 'msedgewebview2.exe').write_bytes(b'new-host-build')
        output = self.root / 'output'
        with self.assertRaises(ValueError):
            runtime.copy_pinned_runtime(self.pin, [self.source], output)
        self.assertFalse(output.exists())

    def test_verified_previous_release_is_used_when_host_changed(self):
        import shutil
        prior = self.root / 'previous-release'
        shutil.copytree(self.source, prior)
        (self.source / 'msedgewebview2.exe').write_bytes(b'new-host-build')
        proof = runtime.copy_pinned_runtime(self.pin, [self.source, prior], self.root / 'output')
        self.assertEqual(pathlib.Path(proof['source']), prior)

    def test_missing_runtime_file_is_rejected(self):
        (self.source / 'Locales/en-US.pak').unlink()
        with self.assertRaises(ValueError):
            runtime.copy_pinned_runtime(self.pin, [self.source], self.root / 'output')

    def test_existing_nonempty_output_cannot_mix_runtime_versions(self):
        output = self.root / 'output'
        output.mkdir()
        (output / 'stale.dll').write_bytes(b'stale')
        with self.assertRaises(ValueError):
            runtime.copy_pinned_runtime(self.pin, [self.source], output)
        self.assertEqual((output / 'stale.dll').read_bytes(), b'stale')

    def test_output_symlink_is_rejected_before_any_copy(self):
        target = self.root / 'untouched'
        target.mkdir()
        output = self.root / 'output-link'
        output.symlink_to(target, target_is_directory=True)
        with self.assertRaises(ValueError):
            runtime.copy_pinned_runtime(self.pin, [self.source], output)
        self.assertEqual(list(target.iterdir()), [])

    def test_output_ancestor_symlink_is_rejected_before_any_creation(self):
        target = self.root / 'untouched'
        target.mkdir()
        ancestor = self.root / 'ancestor-link'
        ancestor.symlink_to(target, target_is_directory=True)
        with self.assertRaises(ValueError):
            runtime.copy_pinned_runtime(self.pin, [self.source], ancestor / 'output')
        self.assertEqual(list(target.iterdir()), [])

    def test_source_ancestor_symlink_is_rejected(self):
        ancestor = self.root / 'source-parent-link'
        ancestor.symlink_to(self.root, target_is_directory=True)
        output = self.root / 'output'
        with self.assertRaises(ValueError):
            runtime.copy_pinned_runtime(self.pin, [ancestor / self.source.name], output)
        self.assertFalse(output.exists())

    def test_unsafe_and_duplicate_manifest_paths_rejected(self):
        original = self.data['files'][0]['path']
        for bad in ['../escape.exe', 'C:/escape.exe', '/escape.exe', 'folder/../escape.exe', 'CON', 'a:ads', 'folder/thing.']:
            with self.subTest(bad=bad):
                self.data['files'][0]['path'] = bad
                self.save()
                with self.assertRaises(ValueError):
                    runtime.copy_pinned_runtime(self.pin, [self.source], self.root / 'output')
        self.data['files'][0]['path'] = original
        self.data['files'].append(dict(self.data['files'][0], path=original.upper()))
        self.save()
        with self.assertRaises(ValueError):
            runtime.copy_pinned_runtime(self.pin, [self.source], self.root / 'output')

    def test_production_pin_is_populated(self):
        pin = runtime.load_pin(ROOT / 'data/webview2-runtime-pin.json')
        self.assertEqual(pin['runtimeVersion'], '157.0.4341.46')
        self.assertEqual(pin['baselineRelease'], '3.3.18')
        self.assertEqual(len(pin['files']), 752)
        self.assertEqual(sorted(x['path'] for x in pin['files'] if x['path'].startswith('Locales/') and x['path'].endswith('.pak')), ['Locales/en-US.pak', 'Locales/zh-CN.pak'])
        self.assertIn('msedgewebview2.exe', {x['path'] for x in pin['files']})


if __name__ == '__main__':
    unittest.main()
