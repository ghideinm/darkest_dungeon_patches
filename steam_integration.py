"""Install a native Steam launch wrapper; Python is only needed during setup."""
from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import tempfile

from launch import (EXPECTED_SHA256, RENDER_SITES, RIG_SITES, RUNTIME_SITES,
                    SITES, build_library, validate)

APP_ID = '262060'
STEAM_ID_BASE = 76561197960265728
HERE = Path(__file__).resolve().parent
APP_PATH = ('UserLocalConfigStore', 'Software', 'Valve', 'Steam', 'apps')
TOKEN = re.compile(r'\s+|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]|[^\s{}"]+')


@dataclass
class Node:
    key: str
    value: str | None
    start: int
    end: int
    close: int = 0
    children: list[Node] = field(default_factory=list)

    def child(self, key):
        matches = [node for node in self.children if node.key.casefold() == key.casefold()]
        if len(matches) > 1:
            raise ValueError(f'Duplicate Steam configuration key: {key}')
        return matches[0] if matches else None


def unquote(value):
    if not value.startswith('"'):
        return value
    return re.sub(r'\\([\\"nrt])', lambda m: {'n': '\n', 'r': '\r', 't': '\t'}.get(
        m[1], m[1]), value[1:-1])


def quote(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace(
        '\n', '\\n').replace('\r', '\\r').replace('\t', '\\t') + '"'


def parse_vdf(text):
    """Keep source spans so unrelated Steam fields and formatting stay intact."""
    tokens = []
    position = 0
    for match in TOKEN.finditer(text):
        if match.start() != position:
            raise ValueError('Malformed Steam configuration')
        position = match.end()
        token = match[0]
        if not token.isspace() and not token.startswith('//'):
            tokens.append((token, match.start(), match.end()))
    if position != len(text):
        raise ValueError('Malformed Steam configuration')
    index = 0

    def entries(nested=False):
        nonlocal index
        result = []
        while index < len(tokens):
            key, _, _ = tokens[index]
            if key == '}':
                if not nested:
                    raise ValueError('Unexpected closing brace')
                return result
            if key == '{' or index + 1 == len(tokens):
                raise ValueError('Missing Steam configuration value')
            value, start, end = tokens[index + 1]
            index += 2
            if value == '{':
                children = entries(True)
                if index == len(tokens) or tokens[index][0] != '}':
                    raise ValueError('Unclosed Steam configuration block')
                node = Node(unquote(key), None, start, tokens[index][2],
                            tokens[index][1], children)
                index += 1
            elif value == '}':
                raise ValueError('Missing Steam configuration value')
            else:
                node = Node(unquote(key), unquote(value), start, end)
            result.append(node)
        if nested:
            raise ValueError('Unclosed Steam configuration block')
        return result

    return Node('', None, 0, len(text), children=entries())


def apps_node(text):
    node = parse_vdf(text)
    for key in APP_PATH:
        node = node.child(key)
        if node is None or node.value is not None:
            raise ValueError(f'Steam apps configuration missing: {key}')
    return node


def launch_options(text):
    app = apps_node(text).child(APP_ID)
    if app and app.value is not None:
        raise ValueError('Steam app configuration must be a block')
    node = app.child('LaunchOptions') if app else None
    if node and node.value is None:
        raise ValueError('Steam LaunchOptions must be a string')
    return node.value if node else None


def replace_launch_options(text, value):
    apps = apps_node(text)
    app = apps.child(APP_ID)
    if app and app.value is not None:
        raise ValueError('Steam app configuration must be a block')
    node = app.child('LaunchOptions') if app else None
    if node:
        if node.value is None:
            raise ValueError('Steam LaunchOptions must be a string')
        # Removing the complete line would also risk deleting a neighbouring
        # field in compact VDF. An empty value restores default launch behavior.
        return text[:node.start] + quote(value or '') + text[node.end:]
    if value is None:
        return text
    if app:
        insert = f'\n\t\t\t\t\t\t"LaunchOptions"\t\t{quote(value)}\n\t\t\t\t\t'
        return text[:app.close] + insert + text[app.close:]
    insert = (f'\n\t\t\t\t\t"{APP_ID}"\n\t\t\t\t\t{{\n'
              f'\t\t\t\t\t\t"LaunchOptions"\t\t{quote(value)}\n'
              '\t\t\t\t\t}\n\t\t\t\t')
    return text[:apps.close] + insert + text[apps.close:]


def default_install_root():
    return Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'darkest_dungeon_patches'


def default_log_path():
    return Path(os.environ.get('XDG_STATE_HOME', Path.home() / '.local/state')) / 'darkest_dungeon_patches/steam_launch.log'


def steam_processes():
    found = []
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid == os.getuid() and (process / 'comm').read_text().strip() == 'steam':
                found.append(int(process.name))
        except (OSError, PermissionError):
            pass
    return found


def select_localconfig(steam_root, user=None):
    if user is None:
        users = parse_vdf((steam_root / 'config/loginusers.vdf').read_text()).child('users')
        if not users:
            raise ValueError('No Steam login found; specify --steam-user')
        candidates = []
        for node in users.children:
            if not node.key.isdigit():
                continue
            recent, timestamp = node.child('MostRecent'), node.child('Timestamp')
            candidates.append((int(bool(recent and recent.value == '1')),
                               int(timestamp.value) if timestamp else 0,
                               str(int(node.key) - STEAM_ID_BASE)))
        if not candidates:
            raise ValueError('No Steam login found; specify --steam-user')
        candidates.sort(reverse=True)
        if len(candidates) > 1 and candidates[0][:2] == candidates[1][:2]:
            raise ValueError('Steam account is ambiguous; specify --steam-user')
        user = candidates[0][2]
    if not str(user).isdigit():
        raise ValueError('--steam-user must be a numeric account ID')
    path = steam_root / 'userdata' / str(user) / 'config/localconfig.vdf'
    if not path.is_file() or path.is_symlink():
        raise ValueError('Steam localconfig.vdf is missing or is a symlink')
    return path.resolve()


def native_config(game, log):
    for path in (game, log):
        if '\n' in str(path) or '\r' in str(path):
            raise ValueError('Newlines are not supported in installation paths')
    sites = [(address, bytes([0xb9 if i == 6 else 0xbf, 0, 0, 0x60, 0]))
             for i, address in enumerate(SITES)] + RENDER_SITES + RIG_SITES + RUNTIME_SITES
    return ('dd_steam_launcher_v1\n' + str(game) + '\n' + str(log) + '\n' +
            EXPECTED_SHA256 + '\n' + ''.join(f'{address:x} {expected.hex()}\n'
                                             for address, expected in sites))


def atomic_write(path, data, mode=0o600):
    if path.is_symlink():
        raise ValueError(f'Refusing symbolic link: {path}')
    fd, temporary = tempfile.mkstemp(prefix='.dd-steam-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
            os.fchmod(stream.fileno(), mode)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def edit_localconfig(path, expected, replacement):
    before = path.read_bytes()
    text = before.decode('utf-8')
    if launch_options(text) != expected:
        raise ValueError('Steam launch options changed; refusing to overwrite them')
    updated = replace_launch_options(text, replacement).encode('utf-8')
    if path.read_bytes() != before:
        raise ValueError('Steam configuration changed during installation')
    if updated != before:
        atomic_write(path, updated, stat.S_IMODE(path.stat().st_mode))


def prepare_release(root, game, log):
    compiler = shutil.which('clang')
    if not compiler or not shutil.which('sha256sum'):
        raise RuntimeError('Installation needs Clang and coreutils sha256sum')
    config = native_config(game, log)
    libraries = [build_library(), build_library('skin_rig_patch.c', 'libdd_skin_rig_patch.so')]
    digest = hashlib.sha256(config.encode() + (HERE / 'steam_launcher.c').read_bytes())
    for library in libraries:
        digest.update(library.read_bytes())
    release = root / 'releases' / digest.hexdigest()[:20]
    release.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.TemporaryDirectory(prefix='dd-steam-build-') as temporary:
        binary = Path(temporary) / 'steam_launcher'
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(HERE / 'steam_launcher.c'), '-o', str(binary)], check=True)
        stage = Path(tempfile.mkdtemp(prefix='.staging-', dir=release.parent))
        try:
            shutil.copy2(binary, stage / binary.name)
            for library in libraries:
                shutil.copy2(library, stage / library.name)
            (stage / 'launcher.conf').write_text(config)
            subprocess.run([str(stage / 'steam_launcher'), '--check'], check=True)
            if release.exists():
                for name in ['steam_launcher', 'launcher.conf', *(p.name for p in libraries)]:
                    if (release / name).is_symlink() or (release / name).read_bytes() != (stage / name).read_bytes():
                        raise ValueError('Installed release differs from the verified build')
            else:
                stage.rename(release)
        finally:
            if stage.exists():
                shutil.rmtree(stage)
    return release


def read_state(root):
    state = root / 'steam_install.json'
    if state.is_symlink():
        raise ValueError('Installation state must not be a symlink')
    if not state.exists():
        return None
    result = json.loads(state.read_text())
    if result.get('version') != 1 or result.get('app_id') != APP_ID:
        raise ValueError('Unrecognized Steam installation state')
    return result


def integrate(action, game, steam_root, install_root, user=None):
    root = install_root.expanduser().absolute()
    if root.is_symlink() or any(c in str(root) for c in ': \t\r\n'):
        raise ValueError('Install root must not be a symlink or contain preload separators')
    root = root.resolve()
    if any(c in str(root) for c in ': \t\r\n'):
        raise ValueError('Resolved install root contains preload separators')
    steam_root = steam_root.expanduser().resolve(strict=True)
    state = read_state(root)
    config_path = select_localconfig(steam_root, user)
    if state and state['localconfig'] != str(config_path):
        raise ValueError('Installation belongs to another Steam account; remove it there first')
    current = launch_options(config_path.read_text())
    if action == 'steam_status':
        active = bool(state and current == state['launch_options'])
        result = {'enabled': active, 'localconfig': str(config_path),
                  'launch_options': current, 'install_root': str(root)}
        if state:
            result['log'] = state['log']
        if active:
            subprocess.run([str(root / 'current/steam_launcher'), '--check'], check=True)
        return result
    if steam_processes():
        raise RuntimeError('Exit Steam normally before changing its launch options')
    if state and current != state['launch_options']:
        raise ValueError('Steam launch options were edited after installation; refusing to overwrite them')
    if action == 'steam_uninstall':
        if not state:
            return {'enabled': False, 'changed': False}
        edit_localconfig(config_path, current, state['previous_launch_options'])
        (root / 'steam_install.json').unlink()
        return {'enabled': False, 'restored_launch_options': state['previous_launch_options'],
                'retained_binaries': str(root)}
    validate(game, native_render=True, runtime_rig=True)
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    if root.stat().st_uid != os.getuid() or root.stat().st_mode & 0o022:
        raise ValueError('Install root must be owned by this user and not writable by other users')
    log = default_log_path().expanduser().absolute()
    log.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    release = prepare_release(root, game, log)
    previous = state['previous_launch_options'] if state else current
    prefix = shlex.quote(str(root / 'current/steam_launcher')) + ' -- '
    if previous and '%command%' in previous:
        if previous.count('%command%') != 1:
            raise ValueError('Steam launch options contain multiple command placeholders')
        options = previous.replace('%command%', prefix + '%command%')
    else:
        options = prefix + '%command%' + (' ' + previous if previous else '')
    new_state = {'version': 1, 'app_id': APP_ID, 'localconfig': str(config_path),
                 'previous_launch_options': previous, 'launch_options': options,
                 'release': str(release), 'log': str(log)}
    link = root / 'current'
    if link.exists() and not link.is_symlink():
        raise ValueError('Installation current entry must be a managed symlink')
    old_link = os.readlink(link) if link.is_symlink() else None
    temporary_link = root / '.current-next'
    temporary_link.symlink_to(release.relative_to(root))
    try:
        os.replace(temporary_link, link)
        # Persist rollback metadata before changing Steam's configuration.
        atomic_write(root / 'steam_install.json', (json.dumps(new_state, indent=2) + '\n').encode())
        edit_localconfig(config_path, current, options)
    except BaseException:
        if old_link is None:
            link.unlink(missing_ok=True)
        else:
            temporary_link.symlink_to(old_link)
            os.replace(temporary_link, link)
        if state:
            atomic_write(root / 'steam_install.json', (json.dumps(state, indent=2) + '\n').encode())
        else:
            (root / 'steam_install.json').unlink(missing_ok=True)
        raise
    finally:
        temporary_link.unlink(missing_ok=True)
    return {'enabled': True, 'localconfig': str(config_path), 'launch_options': options,
            'install_root': str(root), 'release': str(release), 'log': str(log),
            'runtime_python_required': False}
