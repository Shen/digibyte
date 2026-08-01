#!/usr/bin/env python3
# Copyright (c) 2016-2022 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import os
import re
import argparse
from shutil import copyfile

SOURCE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'src'))
DEFAULT_PLATFORM_TOOLSET = R'v143'

libs = [
    'libdigibyte_cli',
    'libdigibyte_common',
    'libdigibyte_consensus',
    'libdigibyte_crypto',
    'libdigibyte_node',
    'libdigibyte_util',
    'libdigibyte_wallet_tool',
    'libdigibyte_wallet',
    'libdigibyte_zmq',
    'bench_digibyte',
    'libtest_util',
]

secp256k1_modules = [
    'RECOVERY',
    'EXTRAKEYS',
    'SCHNORRSIG',
    'ELLSWIFT',
    'MUSIG',
]

ignore_list = [
]

lib_sources = {}


def parse_makefile(makefile):
    with open(makefile, 'r', encoding='utf-8') as file:
        current_lib = ''
        for line in file.read().splitlines():
            if current_lib:
                source = line.split()[0]
                if source.endswith('.cpp') and not source.startswith('$') and source not in ignore_list:
                    source_filename = source.replace('/', '\\')
                    object_filename = source.replace('/', '_')[:-4] + ".obj"
                    lib_sources[current_lib].append((source_filename, object_filename))
                if not line.endswith('\\'):
                    current_lib = ''
                continue
            for lib in libs:
                _lib = lib.replace('-', '_')
                if re.search(_lib + '.*_SOURCES \\= \\\\', line):
                    current_lib = lib
                    lib_sources[current_lib] = []
                    break

def parse_config_into_dgb_config():
    def find_between( s, first, last ):
        try:
            start = s.index( first ) + len( first )
            end = s.index( last, start )
            return s[start:end]
        except ValueError:
            return ""

    config_info = []
    with open(os.path.join(SOURCE_DIR,'../configure.ac'), encoding="utf8") as f:
        for line in f:
            if line.startswith("define"):
                config_info.append(find_between(line, "(_", ")"))

    config_info = [c for c in config_info if not c.startswith("COPYRIGHT_HOLDERS")]

    config_dict = dict(item.split(", ") for item in config_info)
    config_dict["PACKAGE_VERSION"] = f"\"{config_dict['CLIENT_VERSION_MAJOR']}.{config_dict['CLIENT_VERSION_MINOR']}.{config_dict['CLIENT_VERSION_BUILD']}\""
    version = config_dict["PACKAGE_VERSION"].strip('"')
    config_dict["PACKAGE_STRING"] = f"\"DigiByte Core {version}\""

    with open(os.path.join(SOURCE_DIR,'../build_msvc/digibyte_config.h.in'), "r", encoding="utf8") as template_file:
        template = template_file.readlines()

    for index, line in enumerate(template):
        header = ""
        if line.startswith("#define"):
            header = line.split(" ")[1]
        if header in config_dict:
            template[index] = line.replace("$", f"{config_dict[header]}")

    with open(os.path.join(SOURCE_DIR,'../build_msvc/digibyte_config.h'), "w", encoding="utf8") as dgb_config:
        dgb_config.writelines(template)

def set_properties(vcxproj_filename, placeholder, content):
    with open(vcxproj_filename + '.in', 'r', encoding='utf-8') as vcxproj_in_file:
        with open(vcxproj_filename, 'w', encoding='utf-8') as vcxproj_file:
            vcxproj_file.write(vcxproj_in_file.read().replace(placeholder, content))


def parse_makefile_variable(makefile, variable):
    """Return literal entries from all assignments to one make variable."""
    entries = []
    collecting = False
    with open(makefile, 'r', encoding='utf-8') as file:
        for raw_line in file.read().splitlines():
            line = raw_line.strip()
            if not collecting:
                match = re.match(r'^' + re.escape(variable) + r'\s*\+?=\s*(.*)$', line)
                if not match:
                    continue
                line = match.group(1).strip()
            continued = line.endswith('\\')
            if continued:
                line = line[:-1].rstrip()
            if line and not line.startswith('$'):
                entries.extend(entry for entry in line.split() if not entry.startswith('$'))
            collecting = continued
    return entries


def add_missing_items(content, marker, items):
    missing = [item for item in items if item not in content]
    if missing:
        if marker not in content:
            raise RuntimeError('Unexpected MSVC project layout')
        content = content.replace(marker, '\n'.join(missing) + '\n' + marker, 1)
    return content


def sync_qt_test_project():
    makefile = os.path.join(SOURCE_DIR, 'Makefile.qttest.include')
    project = os.path.join(SOURCE_DIR, '../build_msvc/test_digibyte-qt/test_digibyte-qt.vcxproj')
    source_entries = parse_makefile_variable(
        makefile, 'qt_test_test_digibyte_qt_SOURCES')
    moc_entries = parse_makefile_variable(makefile, 'TEST_QT_MOC_CPP')

    with open(project, 'r', encoding='utf-8') as file:
        content = file.read()

    generated_marker = '    <ClCompile Include="$(GeneratedFilesOutDir)\\moc\\moc_addressbooktests.cpp" />'
    source_items = []
    for source in source_entries:
        if source.endswith('.cpp'):
            include = '..\\..\\src\\' + source.replace('/', '\\')
            item = f'    <ClCompile Include="{include}" />'
            # Preserve hand-authored object-name metadata and remove an older
            # simple duplicate if a previous generator run added one.
            if f'<ClCompile Include="{include}">\n' in content:
                content = content.replace(item + '\n', '')
            if f'Include="{include}"' not in content:
                source_items.append(item)
    content = add_missing_items(content, generated_marker, source_items)

    generated_items = []
    moc_header_items = []
    for source in moc_entries:
        if not source.endswith('.cpp'):
            continue
        filename = os.path.basename(source.replace('\\', '/'))
        generated_items.append(
            f'    <ClCompile Include="$(GeneratedFilesOutDir)\\moc\\{filename}" />')
        header = filename.removeprefix('moc_').removesuffix('.cpp') + '.h'
        moc_header_items.append(
            f'    <MocTestFiles Include="..\\..\\src\\qt\\test\\{header}" />')
    generated_end_marker = '  </ItemGroup>\n  <ItemGroup>\n    <ProjectReference'
    content = add_missing_items(content, generated_end_marker, generated_items)

    moc_marker = '    <MocTestFiles Include="..\\..\\src\\qt\\test\\wallettests.h" />'
    content = add_missing_items(content, moc_marker, moc_header_items)

    with open(project, 'w', encoding='utf-8') as file:
        file.write(content)


def sync_qt_project():
    """Synchronize Qt C++ and generated MOC inputs from Makefile.qt.include.

    The historical Qt MSVC project has no .vcxproj.in template, so the normal
    library generation loop cannot update it. Keep the Makefile authoritative
    and make this operation additive and idempotent.
    """
    makefile = os.path.join(SOURCE_DIR, 'Makefile.qt.include')
    project = os.path.join(SOURCE_DIR, '../build_msvc/libdigibyte_qt/libdigibyte_qt.vcxproj')
    source_entries = []
    for variable in ('DIGIBYTE_QT_BASE_CPP', 'DIGIBYTE_QT_WINDOWS_CPP',
                     'DIGIBYTE_QT_WALLET_CPP'):
        source_entries.extend(parse_makefile_variable(makefile, variable))
    moc_entries = parse_makefile_variable(makefile, 'QT_MOC_CPP')

    with open(project, 'r', encoding='utf-8') as file:
        content = file.read()

    generated_marker = '    <ClCompile Include="$(GeneratedFilesOutDir)\\moc\\moc_addressbookpage.cpp" />'
    resource_marker = '    <ClCompile Include="$(GeneratedFilesOutDir)\\rcc\\qrc_digibyte.cpp" />'
    if generated_marker not in content or resource_marker not in content:
        raise RuntimeError('Unexpected libdigibyte_qt.vcxproj layout')

    source_items = []
    for source in source_entries:
        if not source.endswith('.cpp'):
            continue
        include = '..\\..\\src\\' + source.replace('/', '\\')
        item = f'    <ClCompile Include="{include}" />'
        source_items.append(item)
    content = add_missing_items(content, generated_marker, source_items)

    moc_items = []
    for source in moc_entries:
        if not source.endswith('.cpp'):
            continue
        filename = os.path.basename(source.replace('\\', '/'))
        item = f'    <ClCompile Include="$(GeneratedFilesOutDir)\\moc\\{filename}" />'
        moc_items.append(item)
    content = add_missing_items(content, resource_marker, moc_items)

    with open(project, 'w', encoding='utf-8') as file:
        file.write(content)

def main():
    parser = argparse.ArgumentParser(description='DigiByte-core msbuild configuration initialiser.')
    parser.add_argument('-toolset', nargs='?', default=DEFAULT_PLATFORM_TOOLSET,
        help='Optionally sets the msbuild platform toolset, e.g. v143 for Visual Studio 2022.'
         ' default is %s.'%DEFAULT_PLATFORM_TOOLSET)
    args = parser.parse_args()
    set_properties(os.path.join(SOURCE_DIR, '../build_msvc/common.init.vcxproj'), '@TOOLSET@', args.toolset)
    set_properties(
        os.path.join(SOURCE_DIR, '../build_msvc/libsecp256k1/libsecp256k1.vcxproj'),
        '@SECP256K1_MODULES@',
        ';'.join(f'ENABLE_MODULE_{module}' for module in secp256k1_modules),
    )

    for makefile_name in os.listdir(SOURCE_DIR):
        if 'Makefile' in makefile_name:
            parse_makefile(os.path.join(SOURCE_DIR, makefile_name))
    for key, value in lib_sources.items():
        vcxproj_filename = os.path.abspath(os.path.join(os.path.dirname(__file__), key, key + '.vcxproj'))
        content = ''
        for source_filename, object_filename in value:
            content += '    <ClCompile Include="..\\..\\src\\' + source_filename + '">\n'
            content += '      <ObjectFileName>$(IntDir)' + object_filename + '</ObjectFileName>\n'
            content += '    </ClCompile>\n'
        set_properties(vcxproj_filename, '@SOURCE_FILES@\n', content)
    sync_qt_project()
    sync_qt_test_project()
    parse_config_into_dgb_config()
    copyfile(os.path.join(SOURCE_DIR,'../build_msvc/digibyte_config.h'), os.path.join(SOURCE_DIR, 'config/digibyte-config.h'))

if __name__ == '__main__':
    main()
