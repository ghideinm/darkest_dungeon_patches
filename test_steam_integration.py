"""Exercise native launch/refusal and lossless Steam configuration edits in /tmp."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import steam_integration as steam


def localconfig(options=None, app=True):
    entry = ('"262060" { "Playtime" "42" "cloud" { "state" "keep" } ' +
             (f'"LaunchOptions" {steam.quote(options)} ' if options is not None else '') + '}') if app else ''
    return ('// preserve this comment\n"UserLocalConfigStore" { "Software" { "Valve" { '
            '"Steam" { "apps" { "999" { "LaunchOptions" "unrelated" } ' + entry +
            ' } } } } "other" { "262060" "not the app" } }\n')


class ConfigTest(unittest.TestCase):
    def test_changes_only_launch_options_value(self):
        before = localconfig('env FOO="a b" %command% -old')
        value = '/new/steam_launcher -- %command% -arg'
        after = steam.replace_launch_options(before, value)
        self.assertEqual(after, before.replace(steam.quote(steam.launch_options(before)),
                                              steam.quote(value)))
        self.assertEqual(steam.launch_options(after), value)

    def test_inserts_missing_app_or_option_preserving_other_fields(self):
        for app in (False, True):
            with self.subTest(app=app):
                before = localconfig(app=app)
                after = steam.replace_launch_options(before, '/wrapper -- %command%')
                self.assertEqual(steam.launch_options(after), '/wrapper -- %command%')
                self.assertIn('"999" { "LaunchOptions" "unrelated" }', after)
                self.assertIn('"other" { "262060" "not the app" }', after)
                self.assertTrue(after.startswith('// preserve this comment\n'))

    def test_rejects_malformed_or_duplicate_target_fields(self):
        with self.assertRaises(ValueError):
            steam.parse_vdf('"root" { "broken" "x"')
        before = localconfig('one').replace('"LaunchOptions" "one"',
                                          '"LaunchOptions" "one" "LaunchOptions" "two"')
        with self.assertRaises(ValueError):
            steam.launch_options(before)


class InstallationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dd-steam-install-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.steam = self.root / 'steam'
        self.config = self.steam / 'userdata/123/config/localconfig.vdf'
        self.config.parent.mkdir(parents=True)
        self.install = self.root / 'installed'
        self.game = self.root / 'game/_linux/darkest.bin.x86_64'
        self.log = self.root / 'state/steam_launch.log'

    def integrate(self, action, **kwargs):
        return steam.integrate(action, self.game, self.steam, self.install, '123', **kwargs)

    def release(self, root, game, log):
        path = root / 'releases/fixture'
        path.mkdir(parents=True, exist_ok=True)
        return path

    def test_install_update_and_uninstall_keep_previous_options(self):
        for original in (None, '-skipintro', 'FOO=bar gamemoderun %command% -skipintro'):
            with self.subTest(original=original):
                self.config.write_text(localconfig(original))
                with (patch.object(steam, 'validate'),
                      patch.object(steam, 'steam_processes', return_value=[]),
                      patch.object(steam, 'prepare_release', side_effect=self.release),
                      patch.object(steam, 'default_log_path', return_value=self.log)):
                    first = self.integrate('steam_install')
                    options = steam.launch_options(self.config.read_text())
                    self.assertEqual(options.count('%command%'), 1)
                    self.assertEqual(options.count('steam_launcher'), 1)
                    if original and '%command%' in original:
                        self.assertTrue(options.startswith('FOO=bar gamemoderun '))
                    self.assertEqual(self.integrate('steam_install')['launch_options'], options)
                    self.assertFalse(first['runtime_python_required'])
                    self.integrate('steam_uninstall')
                self.assertEqual(steam.launch_options(self.config.read_text()), original or '')
                self.assertFalse((self.install / 'steam_install.json').exists())

    def test_refuses_to_write_while_steam_runs(self):
        before = localconfig()
        self.config.write_text(before)
        with patch.object(steam, 'steam_processes', return_value=[123]):
            with self.assertRaises(RuntimeError):
                self.integrate('steam_install')
        self.assertEqual(self.config.read_text(), before)
        self.assertFalse(self.install.exists())

    def test_rolls_back_current_link_and_state_on_config_failure(self):
        before = localconfig('original')
        self.config.write_text(before)
        with (patch.object(steam, 'validate'),
              patch.object(steam, 'steam_processes', return_value=[]),
              patch.object(steam, 'prepare_release', side_effect=self.release),
              patch.object(steam, 'default_log_path', return_value=self.log),
              patch.object(steam, 'edit_localconfig', side_effect=OSError('fixture'))):
            with self.assertRaises(OSError):
                self.integrate('steam_install')
        self.assertFalse((self.install / 'current').is_symlink())
        self.assertFalse((self.install / 'steam_install.json').exists())
        self.assertEqual(self.config.read_text(), before)

    def test_detects_user_edits_and_selects_recent_account(self):
        self.config.write_text(localconfig('changed'))
        self.install.mkdir()
        (self.install / 'steam_install.json').write_text(json.dumps({
            'version': 1, 'app_id': steam.APP_ID, 'localconfig': str(self.config),
            'launch_options': 'installed'}))
        with patch.object(steam, 'steam_processes', return_value=[]):
            with self.assertRaises(ValueError):
                self.integrate('steam_uninstall')
        login = self.steam / 'config/loginusers.vdf'
        login.parent.mkdir()
        login.write_text('"users" { "76561197960265851" { "Timestamp" "2" } '
                         '"76561197960265852" { "Timestamp" "1" } }')
        self.assertEqual(steam.select_localconfig(self.steam), self.config)


class NativeLauncherTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='dd-steam-native-test-')
        cls.root = Path(cls.temporary.name)
        cls.launcher = cls.root / 'steam_launcher'
        subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(Path(__file__).with_name('steam_launcher.c')),
                        '-o', str(cls.launcher)], check=True)
        source = cls.root / 'fixture.c'
        source.write_text('''#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  puts("FIXTURE_EXECUTED");
  for(int i=0;i<argc;++i) printf("ARG=%s\\n",argv[i]);
  const char *names[]={"DD_LOADBUFFER_PATCH","DD_NATIVE_RENDER","DD_RIG_RUNTIME",
    "DD_RIG_MAP","LD_PRELOAD","FIXTURE_ENV"};
  for(int i=0;i<6;++i) printf("%s=%s\\n",names[i],getenv(names[i])?getenv(names[i]):"<unset>");
  return 0;
}''')
        cls.fixture = cls.root / 'fixture'
        subprocess.run(['clang', str(source), '-o', str(cls.fixture)], check=True)
        source.write_text('int fixture_library(void) { return 1; }\n')
        cls.library = cls.root / 'fixture.so'
        subprocess.run(['clang', '-fPIC', '-shared', str(source), '-o',
                        str(cls.library)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='dd-steam-run-test-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.game = self.directory / 'game with spaces/_linux/darkest.bin.x86_64'
        self.game.parent.mkdir(parents=True)
        shutil.copy2(self.fixture, self.game)
        self.launch = self.directory / 'steam_launcher'
        shutil.copy2(self.launcher, self.launch)
        for name in ('libdd_resource_loading_patch.so', 'libdd_skin_rig_patch.so'):
            shutil.copy2(self.library, self.directory / name)
        self.log = self.directory / 'launch.log'
        data = self.game.read_bytes()
        phoff = struct.unpack_from('<Q', data, 32)[0]
        phsize, phcount = struct.unpack_from('<HH', data, 54)
        for i in range(phcount):
            kind, flags, offset, address, _, size, _, _ = struct.unpack_from(
                '<IIQQQQQQ', data, phoff + i * phsize)
            if kind == 1 and flags & 1 and size >= 8:
                self.site = f'{address:x} {data[offset:offset+8].hex()}'
                break
        self.config = ('dd_steam_launcher_v1\n' + str(self.game) + '\n' + str(self.log) +
                       '\n' + hashlib.sha256(data).hexdigest() + '\n' + self.site + '\n')
        (self.directory / 'launcher.conf').write_text(self.config)
        self.environment = dict(os.environ)
        self.environment.pop('LD_PRELOAD', None)
        self.environment['DD_RIG_MAP'] = '/obsolete/map'

    def run_launcher(self, *args, environment=None):
        result = subprocess.run([str(self.launch), *map(str, args)],
                                env=environment or self.environment, capture_output=True, text=True)
        log = self.log.read_text() if self.log.exists() else ''
        return result, log

    def test_native_check_and_no_python_launch_preserve_arguments_and_environment(self):
        result, _ = self.run_launcher('--check')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.log.exists())
        native_bin = self.directory / 'native_bin'
        native_bin.mkdir()
        (native_bin / 'sha256sum').symlink_to(shutil.which('sha256sum'))
        environment = dict(self.environment, PATH=str(native_bin))
        self.assertIsNone(shutil.which('python', path=environment['PATH']))
        result, log = self.run_launcher('--', '/usr/bin/env', 'FIXTURE_ENV=a b',
                                        self.game, 'literal $(touch forbidden)', 'a b',
                                        environment=environment)
        self.assertEqual(result.returncode, 0, log)
        self.assertIn('FIXTURE_EXECUTED', log)
        self.assertIn('ARG=literal $(touch forbidden)', log)
        self.assertIn('ARG=a b', log)
        self.assertIn('FIXTURE_ENV=a b', log)
        for key in ('DD_LOADBUFFER_PATCH', 'DD_NATIVE_RENDER', 'DD_RIG_RUNTIME'):
            self.assertIn(f'{key}=1', log)
        self.assertIn('DD_RIG_MAP=<unset>', log)
        self.assertIn('libdd_skin_rig_patch.so', log)

    def test_selects_verified_64_bit_game_and_preserves_other_preloads(self):
        sibling = self.game.with_name('darkest.bin.x86')
        sibling.touch()
        other = self.directory / 'other.so'
        shutil.copy2(self.library, other)
        environment = dict(self.environment, LD_PRELOAD=str(other))
        result, log = self.run_launcher('--', sibling, 'argument', environment=environment)
        self.assertEqual(result.returncode, 0, log)
        self.assertIn(f'ARG={self.game}', log)
        self.assertIn(':' + str(other), log)

    def test_rejects_modified_game_or_instruction_before_execution(self):
        bad = self.config.replace(hashlib.sha256(self.game.read_bytes()).hexdigest(), '0' * 64)
        (self.directory / 'launcher.conf').write_text(bad)
        result, log = self.run_launcher('--', self.game)
        self.assertEqual(result.returncode, 125)
        self.assertNotIn('FIXTURE_EXECUTED', log)
        (self.directory / 'launcher.conf').write_text(self.config.replace(
            self.site, self.site.split()[0] + ' 0000000000000000'))
        result, log = self.run_launcher('--', self.game)
        self.assertEqual(result.returncode, 125)
        self.assertIn('DD_STEAM_SITE_REFUSED', log)
        self.assertNotIn('FIXTURE_EXECUTED', log)

    def test_rejects_unrelated_command_and_missing_or_duplicate_patch(self):
        result, _ = self.run_launcher('--', '/usr/bin/true')
        self.assertEqual(result.returncode, 125)
        environment = dict(self.environment, LD_PRELOAD=str(self.directory / 'libdd_skin_rig_patch.so'))
        result, log = self.run_launcher('--', self.game, environment=environment)
        self.assertEqual(result.returncode, 125)
        self.assertIn('duplicate_patch', log)
        (self.directory / 'libdd_skin_rig_patch.so').unlink()
        result, log = self.run_launcher('--', self.game)
        self.assertEqual(result.returncode, 125)
        self.assertNotIn('FIXTURE_EXECUTED', log)


if __name__ == '__main__':
    unittest.main()
