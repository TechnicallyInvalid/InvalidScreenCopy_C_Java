#!/usr/bin/env python3
"""Inspect (never load) Mach-O dependencies, or copy them into an app bundle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def version(value):
    parts = tuple(int(p) for p in value.split("."))
    return parts + (0,) * (3 - len(parts))


def system_path(path):
    return path.startswith(("/System/Library/", "/usr/lib/"))


def metadata(path):
    """otool is read-only and works for binaries that cannot run on this OS."""
    architecture = run("uname", "-m").strip()
    if architecture not in run("lipo", "-archs", str(path)).split():
        raise ValueError(f"{path} does not contain host architecture {architecture}")
    load = run("otool", "-l", str(path))
    minimums = re.findall(r"\bminos\s+([\d.]+)", load)
    minimums += re.findall(
        r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([\d.]+)", load
    )
    if not minimums:
        raise ValueError(f"No macOS deployment target in {path}")
    rpaths = re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", load)
    ids = set(line.strip() for line in run("otool", "-D", str(path)).splitlines()[1:])
    dependencies = []
    for line in run("otool", "-L", str(path)).splitlines():
        match = re.match(r"\s+(.*?) \(compatibility version", line)
        if match and match[1] not in ids:
            dependencies.append(match[1])
    return max(minimums, key=version), tuple(dict.fromkeys(rpaths)), dependencies


def expand(value, loader, executable):
    return value.replace("@loader_path", str(loader.parent)).replace(
        "@executable_path", str(executable.parent)
    )


def resolve(value, loader, executable, rpaths):
    if value.startswith("@rpath/"):
        suffix = value[len("@rpath/"):]
        candidates = [Path(p) / suffix for p in rpaths]
    else:
        candidates = [Path(expand(value, loader, executable))]
    for path in candidates:
        if path.is_absolute() and path.is_file():
            return path.resolve()
    raise ValueError(f"Unresolved dependency {value!r} from {loader}")


def graph(roots, target, required_prefix=None):
    nodes = {}
    visited = set()

    def visit(path, executable, inherited=()):
        path = path.resolve()
        key = (path, executable)
        if key in visited:
            return
        visited.add(key)
        minimum, local_rpaths, deps = metadata(path)
        if version(minimum) > version(target):
            raise ValueError(f"{path} requires macOS {minimum}; target is {target}")
        rpaths = tuple(expand(p, path, executable) for p in local_rpaths) + inherited
        edges = {}
        for dep in deps:
            if system_path(dep):
                continue
            resolved = resolve(dep, path, executable, rpaths)
            if required_prefix and not resolved.is_relative_to(required_prefix):
                raise ValueError(f"Non-private dependency: {resolved}")
            edges[dep] = resolved
            visit(resolved, executable, rpaths)
        previous = nodes.get(path)
        if previous and previous["edges"] != edges:
            raise ValueError(f"Ambiguous runtime dependency resolution: {path}")
        nodes[path] = {"minimum_os": minimum, "rpaths": local_rpaths, "edges": edges}

    for root in roots:
        visit(root, root.resolve())
    return nodes


def bundle(roots, destination, target, prefix):
    nodes = graph(roots, target, prefix)
    destination.mkdir(parents=True, exist_ok=True)
    root_set = {p.resolve() for p in roots}
    copies = {}
    names = {}
    for path in nodes:
        if path in root_set:
            continue
        if path.name in names and names[path.name] != path:
            raise ValueError(f"Conflicting dylib basenames: {path.name}")
        names[path.name] = path
        dest = destination / path.name
        shutil.copy2(path, dest)
        dest.chmod(dest.stat().st_mode | 0o200)
        copies[path] = dest.resolve()
    for path, node in nodes.items():
        output = copies.get(path, path)
        # Remove build-machine search paths; every non-system edge is explicit.
        for rpath in node["rpaths"]:
            run("install_name_tool", "-delete_rpath", rpath, str(output))
        if path in copies:
            run("install_name_tool", "-id", "@loader_path/" + output.name, str(output))
        for original, resolved in node["edges"].items():
            relative = os.path.relpath(copies[resolved], output.parent)
            run("install_name_tool", "-change", original, "@loader_path/" + relative, str(output))
    for path in copies.values():
        run("codesign", "--force", "--sign", "-", str(path))
    checked = graph(roots, target, destination.resolve())
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default=run("sw_vers", "-productVersion").strip())
    parser.add_argument("--require-prefix", type=Path)
    parser.add_argument("--bundle", type=Path, help="Copy and relink non-system libraries here")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("binaries", nargs="+", type=Path)
    args = parser.parse_args()
    prefix = args.require_prefix.resolve() if args.require_prefix else None
    if args.bundle:
        if prefix is None:
            parser.error("--bundle requires --require-prefix")
        nodes = bundle(args.binaries, args.bundle, args.target, prefix)
    else:
        nodes = graph(args.binaries, args.target, prefix)
    rows = []
    roots = {path.resolve() for path in args.binaries}
    for path, node in sorted(nodes.items()):
        print(f"macOS {node['minimum_os']}: {path.name}")
        row = {"file": path.name, "minimum_macos": node["minimum_os"],
               "dependencies": [p.name for p in node["edges"].values()]}
        # The outer .app signing operation changes its entry-point signature
        # to seal this manifest. Including that executable's digest here would
        # therefore be circular. Private dylibs are already signed and remain
        # unchanged by the final (non-recursive) app signing operation.
        if path not in roots:
            row["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        rows.append(row)
    if args.manifest:
        args.manifest.write_text(json.dumps({
            "target_macos": args.target,
            "hash_scope": "SHA256 covers signed dependency dylibs only. Entry-point "
                          "hashes are omitted because final app signing seals this manifest.",
            "files": rows,
        }, indent=2) + "\n")
    print(f"PASS: {len(nodes)} Mach-O files; all non-system dependencies resolved and compatible")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f"Dependency validation failed: {error}")
