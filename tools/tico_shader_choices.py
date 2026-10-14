#!/usr/bin/env python3
"""Refresh the custom shader picker in tico's settings for DraStic.

    tools/tico_shader_choices.py

Lists every .dfx preset in third_party/drastic-ds-shaders (built into the NRO
as romfs:/shaders) as a choice of the "Wrapper/CustomShader" option in
source/tico/module/settings.json, adding the option after "FSR sharpness" the
first time. Run it after adding or removing a preset.
"""
import json
import os

APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADERS = os.path.join(APP, 'third_party', 'drastic-ds-shaders')
SETTINGS = os.path.join(APP, 'source', 'tico', 'module', 'settings.json')
KEY = 'Wrapper/CustomShader'


def label(relative):
    folder, name = os.path.split(relative)
    name = os.path.splitext(name)[0].replace('_', ' ').replace('+', ' + ')
    folder = folder.replace('xbr-sabr', 'xBR/SABR')
    return '%s: %s' % (folder, name) if folder else name


def presets():
    found = []
    for root, _, files in os.walk(SHADERS):
        for name in files:
            if name.lower().endswith('.dfx'):
                found.append(os.path.relpath(os.path.join(root, name), SHADERS).replace(os.sep, '/'))
    # Plain presets first, then each folder's
    return sorted(found, key=lambda p: (p.count('/'), p.lower()))


def options_with(node, key):
    """The options list holding the option @p key, and its index."""
    if isinstance(node, dict):
        for value in node.values():
            found = options_with(value, key)
            if found:
                return found
    elif isinstance(node, list):
        for i, item in enumerate(node):
            if isinstance(item, dict) and item.get('key') == key:
                return node, i
            found = options_with(item, key)
            if found:
                return found
    return None


def main():
    with open(SETTINGS, encoding='utf-8') as f:
        settings = json.load(f)
    choices = [{'label': label(p), 'value': p} for p in presets()]
    existing = options_with(settings, KEY)
    if existing:
        options, index = existing
        options[index]['choices'] = choices
    else:
        options, index = options_with(settings, 'Wrapper/FsrSharpness')
        options.insert(index + 1, {
            'key': KEY,
            'label': 'settings_drastic_custom_shader',
            'type': 'string',
            'default': '',
            'max_length': 255,
            'choices': choices,
            'depends_on': {'key': 'Wrapper/VideoFilter', 'value': 'custom'},
        })
    with open(SETTINGS, 'w', encoding='utf-8') as f:
        f.write(json.dumps(settings, indent=2, ensure_ascii=False) + '\n')
    print('%d presets in %s' % (len(choices), KEY))


if __name__ == '__main__':
    main()
