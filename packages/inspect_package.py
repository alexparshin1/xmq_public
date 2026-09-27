#!/usr/bin/env python3
#
# XMQ Message QUEUE - copyright (c) 1999-2026 by Alexey Parshin.
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
#
"""Static inspection of an XMQ package: the first layer of acceptance.

Reads a .deb, .rpm or FreeBSD .pkg without installing it and checks what a package manager will do
to a machine that already runs XMQ - the "second time" every other test skips:

  1. nothing under etc/xmq but *.template, because a live file there is overwritten on upgrade and
     deleted on removal - which is how 0.9.15 to 0.9.16 lost a FreeBSD operator's settings;
  2. configuration templates are 0640, since they carry credentials;
  3. binaries and rc scripts are 0755;
  4. every file belongs to root;
  5. no conffiles are declared, so the package never takes ownership of configuration;
  6. against the previous release's list: no file vanishes unless it is named as expected, because a
     path the old package had and the new one lacks is deleted on upgrade;
  7. against the previous release's list: no new dependency appears unless it is named as expected -
     the class of change that once dragged 30 packages and 356 MB in behind one library;
  8. no binary carries DT_RUNPATH. RUNPATH is searched after LD_LIBRARY_PATH, and its mere presence
     makes the loader ignore DT_RPATH - so one RUNPATH lets the environment choose the SPTK a broker
     runs with;
  9. every SPTK library a binary needs is in the package, where that binary's RPATH says to look -
     the bundled copy is found by the RPATH and nowhere else, so a library missing from that
     directory, or a binary naming another SPTK release, is a broker that does not start.

Runs with nothing but the tool the package's own distribution ships - dpkg-deb, rpm and rpm2cpio, or
tar for a FreeBSD package, whose +MANIFEST is JSON - so it runs inside the image that built the
package. The ELF headers are read here rather than with readelf, and the rpm payload's cpio archive is
read here rather than with cpio, because neither tool is in every image.

Exit status: 0 clean, 1 a check failed, 2 the package could not be read.
"""

import argparse
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile


class Inventory:
    """What a package contains and declares, independent of its format."""

    def __init__(self):
        self.files = {}          # path -> (kind, mode, owner, group); kind is 'file', 'dir' or 'link'
        self.depends = set()     # dependency names, versions stripped
        self.conffiles = []      # paths the package declares as configuration
        self.elf = {}            # path -> ElfDynamic, for every ELF file in the payload
        self.elf_unread = None   # why the payload could not be read for check 8 and 9, if it could not


class ElfDynamic:
    """The parts of an ELF file's dynamic section the loader uses to find libraries."""

    def __init__(self):
        self.needed = []
        self.rpath = None
        self.runpath = None


def run(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(command)} failed: {result.stderr.strip()}")
    return result.stdout


def normalise(path):
    """Package paths come as ./etc/x, /etc/x or etc/x; compare them as etc/x."""
    path = path.strip()
    if path.startswith('./'):
        path = path[2:]
    return path.lstrip('/')


def mode_from_permissions(text):
    """An ls-style permission string such as -rw-r----- into a numeric mode."""
    kind = {'d': 'dir', 'l': 'link'}.get(text[0], 'file')
    mode = 0
    bits = [(stat.S_IRUSR, stat.S_IWUSR, stat.S_IXUSR, stat.S_ISUID, 's'),
            (stat.S_IRGRP, stat.S_IWGRP, stat.S_IXGRP, stat.S_ISGID, 's'),
            (stat.S_IROTH, stat.S_IWOTH, stat.S_IXOTH, stat.S_ISVTX, 't')]
    for index, (read, write, execute, special, letter) in enumerate(bits):
        triple = text[1 + index * 3: 4 + index * 3]
        if triple[0] == 'r':
            mode |= read
        if triple[1] == 'w':
            mode |= write
        if triple[2] in ('x', letter):
            mode |= execute
        if triple[2] in (letter, letter.upper()):
            mode |= special
    return kind, mode


def dependency_name(token):
    """A dependency without its version: 'libc6 (>= 2.38)' and 'libc.so.6(GLIBC_2.34)(64bit)' alike."""
    token = token.strip()
    token = re.split(r'\s|\(|>=|<=|=|>|<', token, maxsplit=1)[0]
    return token


# --- ELF --------------------------------------------------------------------------------------------

PT_LOAD, PT_DYNAMIC = 1, 2
DT_NULL, DT_NEEDED, DT_STRTAB, DT_RPATH, DT_RUNPATH = 0, 1, 5, 15, 29


def read_elf_dynamic(data):
    """The dynamic section of a 64-bit little-endian ELF file, or None for anything else.

    64-bit little-endian is every platform XMQ packages for (x86-64 and arm64, Linux and FreeBSD); a
    file of another class is not something these packages contain, and is reported as unreadable
    rather than guessed at.
    """
    if len(data) < 64 or data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        return None
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    loads = []
    dynamic = None
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + 56 > len(data):
            return None
        p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz, _memsz, _align = struct.unpack_from('<IIQQQQQQ', data, offset)
        if p_type == PT_LOAD:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == PT_DYNAMIC:
            dynamic = (p_offset, p_filesz)
    result = ElfDynamic()
    if dynamic is None:
        return result

    entries = []
    offset, size = dynamic
    for position in range(offset, min(offset + size, len(data)) - 15, 16):
        tag, value = struct.unpack_from('<qQ', data, position)
        if tag == DT_NULL:
            break
        entries.append((tag, value))

    strtab_address = next((value for tag, value in entries if tag == DT_STRTAB), None)
    if strtab_address is None:
        return result
    strtab_offset = None
    for vaddr, file_offset, filesz in loads:
        if vaddr <= strtab_address < vaddr + filesz:
            strtab_offset = file_offset + (strtab_address - vaddr)
            break
    if strtab_offset is None:
        return result

    def string_at(index):
        start = strtab_offset + index
        end = data.find(b'\0', start)
        return data[start:end if end >= 0 else len(data)].decode('utf-8', 'replace')

    for tag, value in entries:
        if tag == DT_NEEDED:
            result.needed.append(string_at(value))
        elif tag == DT_RPATH:
            result.rpath = string_at(value)
        elif tag == DT_RUNPATH:
            result.runpath = string_at(value)
    return result


def read_elf_tree(root, inventory):
    """Every ELF file under an unpacked payload, keyed by its package path."""
    for directory, _subdirectories, names in os.walk(root):
        for name in names:
            full = os.path.join(directory, name)
            if os.path.islink(full) or not os.path.isfile(full):
                continue
            with open(full, 'rb') as source:
                if source.read(4) != b'\x7fELF':
                    continue
                source.seek(0)
                dynamic = read_elf_dynamic(source.read())
            path = normalise(os.path.relpath(full, root))
            if dynamic is None:
                inventory.elf_unread = f"{path}: not a 64-bit little-endian ELF file"
                continue
            inventory.elf[path] = dynamic


def read_cpio_newc(data, root):
    """Unpack the regular files of a cpio archive in the newc format - what rpm2cpio writes."""
    position = 0
    while position + 110 <= len(data):
        header = data[position:position + 110]
        if header[:6] not in (b'070701', b'070702'):
            raise RuntimeError(f"cpio: unexpected header at offset {position}")
        fields = [int(header[6 + index * 8:14 + index * 8], 16) for index in range(13)]
        mode, file_size, name_size = fields[1], fields[6], fields[11]
        name_start = position + 110
        name = data[name_start:name_start + name_size - 1].decode('utf-8', 'replace')
        data_start = (name_start + name_size + 3) & ~3
        if name == 'TRAILER!!!':
            break
        if stat.S_ISREG(mode):
            target = os.path.join(root, normalise(name))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, 'wb') as output:
                output.write(data[data_start:data_start + file_size])
        position = (data_start + file_size + 3) & ~3


def read_payload(path, inventory):
    """Unpack the package into a scratch directory and read its ELF files, for checks 8 and 9."""
    with tempfile.TemporaryDirectory(prefix='inspect-package-') as root:
        try:
            if path.endswith('.deb'):
                run(['dpkg-deb', '-x', path, root])
            elif path.endswith('.rpm'):
                result = subprocess.run(['rpm2cpio', path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if result.returncode != 0:
                    raise RuntimeError(f"rpm2cpio failed: {result.stderr.decode(errors='replace').strip()}")
                read_cpio_newc(result.stdout, root)
            elif path.endswith('.pkg'):
                run(['tar', '-xf', path, '-C', root])
            read_elf_tree(root, inventory)
        except (RuntimeError, OSError) as error:
            inventory.elf_unread = str(error)


# --- package formats -------------------------------------------------------------------------------

def read_deb(path):
    inventory = Inventory()
    for line in run(['dpkg-deb', '-c', path]).splitlines():
        # -rw-r----- root/root   1682 2026-09-08 10:00 ./etc/xmq/xmq_server.conf.template
        parts = line.split(None, 5)
        if len(parts) < 6:
            continue
        kind, mode = mode_from_permissions(parts[0])
        owner, _, group = parts[1].partition('/')
        name = parts[5].split(' -> ')[0]
        name = normalise(name)
        if name:
            inventory.files[name] = (kind, mode, owner, group)

    depends = run(['dpkg-deb', '-f', path, 'Depends']).strip()
    for clause in depends.split(','):
        for alternative in clause.split('|'):
            name = dependency_name(alternative)
            if name:
                inventory.depends.add(name)

    members = run(['sh', '-c', f"dpkg-deb --ctrl-tarfile '{path}' | tar -t"]).split()
    if any(normalise(member) == 'conffiles' for member in members):
        inventory.conffiles = [normalise(line) for line in run(['dpkg-deb', '-I', path, 'conffiles']).splitlines()
                               if line.strip()]
    return inventory


def read_rpm(path):
    inventory = Inventory()
    query = '[%{FILEMODES:perms}\t%{FILEUSERNAME}\t%{FILEGROUPNAME}\t%{FILENAMES}\n]'
    for line in run(['rpm', '-qp', '--qf', query, path]).splitlines():
        parts = line.split('\t')
        if len(parts) != 4:
            continue
        kind, mode = mode_from_permissions(parts[0])
        inventory.files[normalise(parts[3])] = (kind, mode, parts[1], parts[2])

    for line in run(['rpm', '-qRp', path]).splitlines():
        if line.startswith('rpmlib(') or not line.strip():
            continue
        inventory.depends.add(dependency_name(line))

    configured = run(['rpm', '-qcp', path])
    inventory.conffiles = [normalise(line) for line in configured.splitlines()
                           if line.strip() and not line.startswith('(')]
    return inventory


def read_freebsd_pkg(path):
    inventory = Inventory()
    manifest = json.loads(run(['tar', '-xOf', path, '+MANIFEST']))
    for name, attributes in (manifest.get('files') or {}).items():
        mode = int(str(attributes.get('perm', '0')), 8)
        inventory.files[normalise(name)] = ('file', mode, attributes.get('uname', ''), attributes.get('gname', ''))
    for name in (manifest.get('directories') or {}):
        inventory.files[normalise(name)] = ('dir', 0o755, 'root', 'wheel')
    inventory.depends.update((manifest.get('deps') or {}).keys())
    inventory.depends.update(manifest.get('shlibs_required') or [])
    inventory.conffiles = [normalise(name) for name in (manifest.get('config') or [])]
    return inventory


def read_package(path):
    if path.endswith('.deb'):
        inventory = read_deb(path)
    elif path.endswith('.rpm'):
        inventory = read_rpm(path)
    elif path.endswith('.pkg'):
        inventory = read_freebsd_pkg(path)
    else:
        raise RuntimeError(f"{path}: not a .deb, .rpm or .pkg")
    read_payload(path, inventory)
    return inventory


# --- checks ----------------------------------------------------------------------------------------

def path_family(path):
    """A path with the parts that change every release taken out, so a rename is not a removal.

    Three kinds of file are renamed by every build and are not deleted by an upgrade in any sense
    that matters: the bundled SPTK libraries carry the SPTK version in their names (libspdb5.so.5.6.10
    becomes libspdb5.so.5.6.11), the web interface's bundles carry a content hash (main.b6cbca37.js),
    and rpm's build-id links are named after a hash of the binary they point at, so every one of them
    changes whenever anything is recompiled. Compared by exact name, every release would fail on them,
    and a check that always fails is one nobody reads - the 0.9.17 packages for EL9 and EL10 reported
    six such deletions and one real finding, which is the ratio that teaches people to skip the
    output.
    """
    path = re.sub(r'(\.so)(\.\d+)+$', r'\1.*', path)
    path = re.sub(r'\.[0-9a-f]{8}(?=\.(js|css|js\.LICENSE\.txt|map)$)', '.*', path)
    path = re.sub(r'(\.build-id/)[0-9a-f]{2}/[0-9a-f]+$', r'\1*', path)
    return path


def in_configuration(path):
    return re.search(r'(^|/)etc/xmq/', path) is not None


def is_executable_location(path):
    return re.search(r'(^|/)(s?bin|etc/rc\.d)/[^/]+$', path) is not None


def is_sptk_library(name):
    return re.match(r'lib(sputil|spdb|spwsdl|sptk)5(_\w+)?\.so', name) is not None


def resolve_needed(binary_path, rpath, name, files):
    """The package path where the loader finds a library by the binary's RPATH, or None."""
    directory = os.path.dirname(binary_path)
    for entry in (rpath or '').split(':'):
        if not entry:
            continue
        entry = entry.replace('${ORIGIN}', '$ORIGIN')
        if entry.startswith('$ORIGIN'):
            candidate = os.path.normpath(os.path.join(directory, entry[len('$ORIGIN'):].lstrip('/'), name))
        else:
            candidate = normalise(os.path.join(entry, name))
        if files.get(candidate, ('',))[0] == 'file':
            return candidate
    return None


def inspect(inventory, baseline, expected_removals, expected_dependencies):
    failures = []
    notes = []

    for path, (kind, mode, owner, _group) in sorted(inventory.files.items()):
        if kind == 'dir':
            continue
        if in_configuration(path):
            if not path.endswith('.template'):
                failures.append(f"{path}: a live file under etc/xmq - upgrade overwrites it and removal deletes it; "
                                "ship it as a .template")
            elif kind == 'file' and (mode & 0o777) != 0o640:
                failures.append(f"{path}: mode {mode & 0o777:04o}, a configuration template must be 0640")
        if kind == 'file' and is_executable_location(path) and (mode & 0o777) != 0o755:
            failures.append(f"{path}: mode {mode & 0o777:04o}, an executable must be 0755")
        if owner not in ('root', '0'):
            failures.append(f"{path}: owned by '{owner}', every packaged file must belong to root")

    for path in inventory.conffiles:
        failures.append(f"{path}: declared as a conffile - the package must not own configuration")

    if inventory.elf_unread:
        failures.append(f"payload not inspected for RUNPATH and bundled SPTK: {inventory.elf_unread}")
    for path, dynamic in sorted(inventory.elf.items()):
        if dynamic.runpath is not None:
            failures.append(f"{path}: DT_RUNPATH '{dynamic.runpath}' - searched after LD_LIBRARY_PATH, and it makes "
                            "the loader ignore DT_RPATH; link with -Wl,--disable-new-dtags")
        for name in dynamic.needed:
            if not is_sptk_library(name):
                continue
            found = resolve_needed(path, dynamic.rpath, name, inventory.files)
            if found is None:
                failures.append(f"{path}: needs {name}, which is not in the package where its RPATH "
                                f"'{dynamic.rpath or ''}' looks")

    if baseline is not None:
        present = {path for path, (kind, *_rest) in inventory.files.items() if kind != 'dir'}
        previous_files = set(baseline.get('files', []))
        present_families = {path_family(path) for path in present}
        previous_families = {path_family(path) for path in previous_files}
        for path in sorted(previous_files - present):
            if path_family(path) in present_families:
                notes.append(f"{path}: renamed by the build (versioned library or hashed bundle)")
            elif path in expected_removals:
                notes.append(f"{path}: removed since the previous release, as expected")
            else:
                failures.append(f"{path}: in the previous release and not in this one - an upgrade deletes it. "
                                "Name it in the expected removals if that is intended")
        for path in sorted(present - previous_files):
            if path_family(path) not in previous_families:
                notes.append(f"{path}: new in this release")

        previous = set(baseline.get('depends', []))
        for name in sorted(inventory.depends - previous):
            if name in expected_dependencies:
                notes.append(f"dependency {name}: new, as expected")
            else:
                failures.append(f"dependency {name}: new in this release. Name it as expected if it is meant to be")
        for name in sorted(previous - inventory.depends):
            notes.append(f"dependency {name}: no longer required")

    return failures, notes


def read_list(path):
    if not path:
        return set()
    with open(path) as source:
        return {line.strip() for line in source if line.strip() and not line.lstrip().startswith('#')}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('package', help='the .deb, .rpm or .pkg to inspect')
    parser.add_argument('--baseline', help="the previous release's list, as written by --write-baseline")
    parser.add_argument('--write-baseline', metavar='FILE', help='write this package as the baseline for the next release')
    parser.add_argument('--expected-removals', metavar='FILE', help='paths allowed to disappear, one per line')
    parser.add_argument('--expected-dependencies', metavar='FILE', help='dependencies allowed to appear, one per line')
    arguments = parser.parse_args()

    try:
        inventory = read_package(arguments.package)
    except (RuntimeError, OSError, ValueError) as error:
        print(f"CANNOT READ {arguments.package}: {error}")
        return 2

    if arguments.write_baseline:
        baseline = {'package': arguments.package.rsplit('/', 1)[-1],
                    'files': sorted(path for path, (kind, *_rest) in inventory.files.items() if kind != 'dir'),
                    'depends': sorted(inventory.depends)}
        with open(arguments.write_baseline, 'w') as target:
            json.dump(baseline, target, indent=2)
            target.write('\n')
        print(f"baseline written: {arguments.write_baseline} ({len(baseline['files'])} files, "
              f"{len(baseline['depends'])} dependencies)")

    baseline = None
    if arguments.baseline:
        with open(arguments.baseline) as source:
            baseline = json.load(source)

    failures, notes = inspect(inventory, baseline,
                              read_list(arguments.expected_removals), read_list(arguments.expected_dependencies))

    name = arguments.package.rsplit('/', 1)[-1]
    for note in notes:
        print(f"  note  {note}")
    for failure in failures:
        print(f"  FAIL  {failure}")
    files = sum(1 for kind, *_rest in inventory.files.values() if kind != 'dir')
    print(f"{name}: {files} files, {len(inventory.depends)} dependencies, {len(inventory.elf)} ELF files, "
          f"{len(failures)} failure(s){'' if baseline is not None else ', no baseline given'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
