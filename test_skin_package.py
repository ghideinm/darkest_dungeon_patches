"""Offline regressions using synthetic packages and game trees in /tmp."""
import copy
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from build_skin_package import build, validate_installed
from manage import install_package
from skin_config import MANIFEST, load_config, read_rig_map, route_rows


def snapshot(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}


class SkinPackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dd-skin-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.game = self.root / 'game'
        self.release = self.root / 'release'
        self.release.mkdir()
        (self.release / 'project.xml').write_text(json.dumps({'Title': 'Fixture Skins'}))
        heroes = []
        for hero, actions in [('alpha', ['idle', 'walk']), ('beta', ['idle', 'walk', 'combat'])]:
            original = self.game / 'heroes' / hero
            custom = self.release / 'heroes' / hero
            for skin in ['A', 'B']:
                (custom / f'{hero}_{skin}').mkdir(parents=True)
            for base, textures in [(original, original / f'{hero}_A/anim'),
                                   (custom, custom / 'anim')]:
                (base / 'anim').mkdir(parents=True, exist_ok=True)
                textures.mkdir(parents=True, exist_ok=True)
                for action in actions:
                    name = f'common.{action}'
                    (base / 'anim' / f'{name}.skel').write_bytes(f'{hero}:{base.name}:{action}'.encode())
                    pages = [f'{name}.png', f'{name}_2.tga']
                    (base / 'anim' / f'{name}.atlas').write_text(''.join(
                        f'\n{page}\nsize: 8,8\nformat: RGBA8888\nregion\n  rotate: false\n'
                        for page in pages))
                    for page in pages:
                        (textures / page).write_bytes(f'{base}:{page}'.encode())
            heroes.append({'id': hero, 'groups': [
                {'id': 'vanilla', 'prefix': 'common.rig_vanilla.', 'skins': [f'{hero}_A'],
                 'assets': {'root': 'game', 'anim': f'heroes/{hero}/anim',
                            'textures': f'heroes/{hero}/{hero}_A/anim', 'prefix': 'common.'}},
                {'id': 'custom', 'prefix': 'common.', 'skins': [f'{hero}_B']}]})
        self.config = {'source': 'release', 'destination': 'mods/fixture', 'heroes': heroes}
        self.config_path = self.root / 'config.json'
        self.write_config()

    def write_config(self):
        self.config_path.write_text(json.dumps(self.config))

    def stage(self):
        return build(self.root / 'stage', self.root, self.game, self.config_path)

    def test_multiple_heroes_shared_names_variable_actions_and_pages(self):
        before = snapshot(self.release), snapshot(self.game)
        package = self.stage()
        config = load_config(self.config_path)
        validate_installed(config, package)
        self.assertEqual(read_rig_map(package.parent / 'skin_rig_map.txt'), route_rows(config))
        for hero, count in [('alpha', 2), ('beta', 3)]:
            anim = package / 'heroes' / hero / 'anim'
            self.assertEqual(len(list(anim.glob('*.skel'))), count * 2)
            atlas = (anim / 'common.rig_vanilla.idle.atlas').read_text()
            self.assertIn('rig_vanilla/common.idle.png', atlas)
            self.assertIn('rig_vanilla/common.idle_2.tga', atlas)
            self.assertEqual((anim / 'rig_vanilla/common.idle_2.tga').read_bytes(),
                             (self.game / f'heroes/{hero}/{hero}_A/anim/common.idle_2.tga').read_bytes())
        self.assertEqual(before, (snapshot(self.release), snapshot(self.game)))

    def test_installed_drift_and_configuration_mismatch(self):
        package = self.stage()
        changed = copy.deepcopy(self.config)
        changed['heroes'][0]['groups'][0]['skins'].append('alpha_C')
        with self.assertRaisesRegex(ValueError, 'configuration differs'):
            validate_installed(changed, package)
        target = package / 'heroes/alpha/anim/common.idle.skel'
        target.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'asset differs'):
            validate_installed(self.config, package)
        target.unlink()
        with self.assertRaisesRegex(ValueError, 'file list differs'):
            validate_installed(self.config, package)

    def use_automatic_discovery(self):
        del self.config['heroes']
        for hero in ['alpha', 'beta']:
            shutil.rmtree(self.release / f'heroes/{hero}/{hero}_A')
        self.write_config()

    def test_automatic_discovery_uses_mod_origin_and_native_fallback(self):
        self.use_automatic_discovery()
        package = self.stage()
        # Installed metadata is sufficient after moving the original release.
        self.release.rename(self.root / 'moved_release')
        resolved = validate_installed(self.config, package, self.game)
        rows = read_rig_map(package.parent / 'skin_rig_map.txt')
        self.assertEqual(rows, route_rows(resolved))
        for hero in ['alpha', 'beta']:
            self.assertIn((f'heroes/{hero}/{hero}_A', 'vanilla', f'{hero}.sprite.rig_vanilla.'), rows)
            self.assertIn((f'heroes/{hero}/{hero}_B', 'mod', 'common.'), rows)

    def test_automatic_discovery_prefers_skin_local_skeletons(self):
        self.use_automatic_discovery()
        local = self.release / 'heroes/alpha/alpha_B/anim'
        shutil.copytree(self.release / 'heroes/alpha/anim', local)
        package = self.stage()
        rows = read_rig_map(package.parent / 'skin_rig_map.txt')
        self.assertIn(('heroes/alpha/alpha_B', 'skin_alpha_B', 'alpha.sprite.rig_alpha_B.'), rows)
        self.assertTrue((package / 'heroes/alpha/anim/alpha.sprite.rig_alpha_B.idle.skel').is_file())
        validate_installed(self.config, package, self.game)

    def test_automatic_discovery_texture_only_skin_reuses_native_rig(self):
        self.use_automatic_discovery()
        for skeleton in (self.release / 'heroes/alpha/anim').glob('*.skel'):
            skeleton.unlink()
        package = self.stage()
        rows = read_rig_map(package.parent / 'skin_rig_map.txt')
        self.assertIn(('heroes/alpha/alpha_B', 'vanilla', 'alpha.sprite.rig_vanilla.'), rows)
        validate_installed(self.config, package, self.game)

    def test_missing_textures_and_conflicting_resources_fail(self):
        texture = self.game / 'heroes/alpha/alpha_A/anim/common.idle_2.tga'
        texture.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing asset'):
            self.stage()
        shutil.rmtree(self.root / 'stage')
        texture.write_bytes(b'restored')
        (self.release / 'heroes/alpha/anim/common.rig_vanilla.idle.skel').write_bytes(b'conflict')
        with self.assertRaisesRegex(ValueError, 'Conflicting asset'):
            self.stage()

    def test_invalid_routes_and_paths_fail(self):
        mutations = []
        duplicate = copy.deepcopy(self.config)
        duplicate['heroes'][0]['groups'][1]['skins'] = ['alpha_A']
        mutations.append(duplicate)
        ambiguous = copy.deepcopy(self.config)
        ambiguous['heroes'][0]['groups'][1]['prefix'] = 'common.rig_vanilla.'
        mutations.append(ambiguous)
        escape = copy.deepcopy(self.config)
        escape['heroes'][0]['groups'][0]['assets']['textures'] = '../outside'
        mutations.append(escape)
        for config in mutations:
            with self.subTest(config=config):
                self.config_path.write_text(json.dumps(config))
                with self.assertRaises(ValueError):
                    load_config(self.config_path)

    def test_original_source_override_and_routed_source_rejected(self):
        self.config['source'] = 'absent'
        self.write_config()
        package = build(self.root / 'stage', self.root, self.game, self.config_path,
                        source=self.release)
        self.assertTrue((package / MANIFEST).is_file())
        with self.assertRaisesRegex(ValueError, 'original Mod release'):
            build(self.root / 'second_stage', self.root, self.game, self.config_path, source=package)

    @patch('manage.ensure_game_stopped')
    def test_install_restore_and_failed_copy_rolls_back(self, stopped):
        package = self.stage()
        destination = self.root / 'installed'
        install_package(package, destination)
        validate_installed(self.config, destination)
        before = snapshot(destination)
        def fail_copy(source, target):
            target.mkdir()
            (target / 'partial').write_bytes(b'incomplete copy')
            raise OSError('simulated copy failure')

        with patch('manage.shutil.copytree', side_effect=fail_copy):
            with self.assertRaisesRegex(OSError, 'simulated'):
                install_package(self.release, destination)
        self.assertEqual(before, snapshot(destination))
        with patch('manage.shutil.which', return_value=None):
            result = install_package(self.release, destination)
        self.addCleanup(shutil.rmtree, Path(result['previous_package']).parent)
        self.assertEqual(snapshot(self.release), snapshot(destination))
        self.assertFalse((destination / MANIFEST).exists())
        self.assertTrue(stopped.called)

    @patch('manage.ensure_game_stopped')
    def test_failed_directory_switch_restores_previous_package(self, stopped):
        destination = self.root / 'installed'
        install_package(self.stage(), destination)
        before = snapshot(destination)
        rename = Path.rename

        def fail_switch(path, target):
            if path.name == 'package' and path.parent.name.startswith('.dd-skin-install-'):
                raise OSError('simulated switch failure')
            return rename(path, target)

        with patch.object(Path, 'rename', new=fail_switch):
            with self.assertRaisesRegex(OSError, 'simulated switch'):
                install_package(self.release, destination)
        self.assertEqual(before, snapshot(destination))
        self.assertEqual(list(self.root.glob('.dd-skin-install-*')), [])
        self.assertTrue(stopped.called)


if __name__ == '__main__':
    unittest.main()
