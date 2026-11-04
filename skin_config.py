"""Shared package configuration and the native three-column route format."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
import re

DEFAULT_CONFIG = Path(__file__).with_name('skin_package.json')
MANIFEST = 'skin_package_manifest.json'


def relative_path(value):
    if not isinstance(value, str) or not value or '\\' in value:
        raise ValueError(f'Expected a relative POSIX path: {value!r}')
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {'', '.', '..'} for part in value.split('/')):
        raise ValueError(f'Unsafe relative path: {value!r}')
    return path


def token(value, limit):
    if (not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*', value)
            or len(value.encode()) > limit):
        raise ValueError(f'Invalid route token: {value!r}')
    return value


def route_rows(config):
    return [(f"heroes/{hero['id']}/{skin}", group['id'], group['prefix'])
            for hero in config['heroes'] for group in hero['groups'] for skin in group['skins']]


def validate_routes(rows):
    if not 1 <= len(rows) <= 128:
        raise ValueError('A route map must contain 1–128 skins')
    skins, groups, prefixes = set(), {}, {}
    for skin, group, prefix in rows:
        parts = relative_path(skin).parts
        if len(parts) != 3 or parts[0] != 'heroes' or len(skin.encode()) > 255:
            raise ValueError(f'Expected heroes/<hero>/<skin>: {skin}')
        token(parts[1], 255)
        token(parts[2], 255)
        token(group, 63)
        token(prefix, 159)
        if skin in skins:
            raise ValueError(f'Duplicate skin route: {skin}')
        skins.add(skin)
        scope = parts[1]
        if groups.setdefault((scope, group), prefix) != prefix:
            raise ValueError(f'One group must use one prefix: {scope}/{group}')
        if prefixes.setdefault((scope, prefix), group) != group:
            raise ValueError(f'A prefix belongs to multiple groups: {scope}/{prefix}')
    return rows


def load_config(path=DEFAULT_CONFIG):
    config = json.loads(Path(path).read_text())
    if (not isinstance(config, dict) or not {'source', 'destination'} <= set(config)
            or set(config) - {'source', 'destination', 'heroes'}):
        raise ValueError('Expected source, destination and optional heroes in package configuration')
    if not isinstance(config['source'], str) or not config['source']:
        raise ValueError('Package source must be a path relative to --project-root, or absolute')
    destination = relative_path(config['destination'])
    if len(destination.parts) != 2 or destination.parts[0] != 'mods':
        raise ValueError('Package destination must be mods/<directory>')
    if 'heroes' not in config:
        return config
    if not isinstance(config['heroes'], list) or not config['heroes']:
        raise ValueError('Configure at least one hero')
    heroes = set()
    for hero in config['heroes']:
        if not isinstance(hero, dict) or set(hero) != {'id', 'groups'}:
            raise ValueError('Each hero needs id and groups')
        identifier = token(hero['id'], 255)
        if identifier in heroes:
            raise ValueError(f'Duplicate hero: {identifier}')
        heroes.add(identifier)
        if not isinstance(hero['groups'], list) or not hero['groups']:
            raise ValueError(f'Configure at least one group for {identifier}')
        group_ids = set()
        for group in hero['groups']:
            if (not isinstance(group, dict) or not {'id', 'prefix', 'skins'} <= set(group)
                    or set(group) - {'id', 'prefix', 'skins', 'assets'}):
                raise ValueError('Each group needs id, prefix, skins and optional assets')
            token(group['id'], 63)
            if group['id'] in group_ids:
                raise ValueError(f'Duplicate group in {identifier}: {group["id"]}')
            group_ids.add(group['id'])
            if not isinstance(group['skins'], list) or not group['skins']:
                raise ValueError('Each group needs at least one skin')
            for skin in group['skins']:
                token(skin, 255)
            if 'assets' in group:
                assets = group['assets']
                if (not isinstance(assets, dict) or set(assets) != {'root', 'anim', 'textures', 'prefix'}
                        or assets['root'] not in {'game', 'source', 'project', 'config'}):
                    raise ValueError('Assets need root (game/source/project/config), anim, textures and prefix')
                relative_path(assets['anim'])
                relative_path(assets['textures'])
                token(assets['prefix'], 159)
    validate_routes(route_rows(config))
    return config


def config_digest(config):
    return hashlib.sha256(json.dumps(config, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def write_rig_map(config, path):
    Path(path).write_text(''.join(f'{skin} {group} {prefix}\n' for skin, group, prefix in route_rows(config)))


def read_rig_map(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        fields = line.split()
        if len(fields) != 3:
            raise ValueError('Each route must contain skin, group and prefix')
        rows.append(tuple(fields))
    return validate_routes(rows)


def group_for_name(hero, name):
    return max((group for group in hero['groups'] if name.startswith(group['prefix'])),
               key=lambda group: len(group['prefix']), default=None)
