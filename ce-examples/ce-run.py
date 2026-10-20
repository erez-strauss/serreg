#!/usr/bin/env python3
"""Compile and run a ce-exampleN directory on Compiler Explorer (godbolt.org REST API).

Usage:  ./ce-run.py [example-dir] [lane | compiler-id ...]
        ./ce-run.py                          # the three lanes, on the directory of this script
        ./ce-run.py ../ce-example2 gcc       # one lane
        ./ce-run.py . edg-trunk-reflection   # any other Compiler Explorer compiler id, with guessed flags
        ./ce-run.py --short-link --project "serreg frames" ../ce-example3
                                             # also save ONE shareable multi-file project with every lane in it

Options:
  --short-link    Upload the example once as a Compiler Explorer multi-file project (a "tree") with one compiler
                  pane and one executor pane per lane, and print its short link (https://godbolt.org/z/...).
                  main.cpp is uploaded as example.cpp: Compiler Explorer recognises the main source of a
                  non-CMake tree by that name.
                  The link is stored by Compiler Explorer permanently and is public to anyone who has it.
  --project NAME  Name the uploaded project. Compiler Explorer's saved state has no project-name field, so NAME
                  goes in a first-line comment of main.cpp and in a PROJECT.md file of the tree.
  --layout L      Pane layout of the saved project (default: tabs).
                    tabs     one column, 60/40: the file tree, example.cpp and every --open file as tabs of the
                             upper stack (example.cpp in front), the compiler and executor panes of every lane
                             as tabs of the lower stack (compiler flags view open; executor with its arguments
                             and stdin panels, compiler output hidden) — the arrangement of the talk's example-6
                             link (godbolt.org/z/EM4aY3qWP), so nothing needs rearranging for a demo;
                    side     the tree + editor on the left, compiler panes top right, executors bottom right;
                    default  Compiler Explorer's own arrangement (the state is posted in its normalized
                             sessions/trees form and the site lays it out).
                  A chosen layout is posted as a GoldenLayout config: rows, columns and stacks with explicit
                  width/height percentages, each pane a component (codeEditor, tree, compiler, executor).
  --open FILE     Also open this tree file in an editor tab (repeatable; paths as in the tree, e.g.
                  es/serreg/reflect.hpp or ce_registry_stub.hpp). example.cpp is always open.
  --asm-flags F   Flags of the compiler (assembly) panes instead of the lane's run flags; the executor panes keep
                  the lane's flags. Default: "-O2 -Wall -Wextra -Wshadow -I." after the lane's -std/-freflection
                  part, so the assembly pane shows an optimised build with the warning set.
  --no-run        Do not compile and run on the API; with --short-link, only save the project (seconds — the
                  shortener stores the files without compiling; the panes compile when the link is opened).
  --verbose       Print each step as it happens (what is uploaded, to which compiler, how long the server took,
                  the HTTP status of a failure). Without it the script is silent while a request is in flight.
  --timeout S     Seconds to wait for one compile+run request (default 600 = 10 minutes: uploads have taken more than 5; the shortener gets 120).

What "hangs" usually is: with no lane argument the script runs ALL THREE lanes, one request each, and a cold
clang-p2996 compile of a reflection-heavy example on the shared server can take 1–3 minutes before the server
answers; a request that gets no answer is cut after --timeout seconds. Name the lane: `./ce-run.py . gcc`.
Note also that `.` is the example DIRECTORY (optional when run from inside it) and the words after it are lanes.

Lanes (directions-log B-O3, maintainer 2026-09-30: GCC and clang now; EDG and Windows much later):
  gcc             x86-64 gcc 16.2 + libstdc++                (g162)
  clang-libcxx    Bloomberg clang P2996 + libc++             (clang_bb_p2996)
  clang-libstdcxx Bloomberg clang P2996 + GCC 16.2 libstdc++ (clang_bb_p2996 --gcc-toolchain=...)
                  NOT POSSIBLE YET: libstdc++'s <meta> only declares the metafunctions (GCC implements them
                  as front-end intrinsics) and is enabled only when __cpp_impl_reflection >= 202603L, which
                  clang-p2996 does not define. The lane stays in the list as a probe: it reports
                  "unavailable" until a clang can build <meta> from libstdc++.

Sends main.cpp as the source and every header under es/ (symlinks resolved) as extra files, so
the `#include <es/serreg/...>`, `<es/ipc/...>` lines resolve with -I. on the server. Prints each compiler's diagnostics, the program
output and its exit code; exits 0 only if every compiler built and ran the example with exit code 0.
Compiler ids: `curl -s https://godbolt.org/api/compilers/c++ | grep -i -e p2996 -e 'gcc 16'`.
"""
import argparse
import json
import pathlib
import sys
import time
import urllib.error
import urllib.request

VERBOSE = False
TIMEOUT = 600


def log(msg: str) -> None:
    if VERBOSE:
        print(f"  [{time.strftime('%H:%M:%S')}] {msg}", flush=True)

API = "https://godbolt.org/api/compiler/{}/compile"
SHORTENER = "https://godbolt.org/api/shortener"
GCC_TOOLCHAIN = "/opt/compiler-explorer/gcc-16.2.0"  # the GCC whose libstdc++ the clang-libstdcxx lane uses
LANES = {  # lane -> (Compiler Explorer compiler id, flags)
    "gcc": ("g162", "-std=c++26 -freflection -O1 -I."),
    "clang-libcxx": ("clang_bb_p2996", "-std=c++26 -freflection-latest -stdlib=libc++ -O1 -I."),
    "clang-libstdcxx": ("clang_bb_p2996",
                        f"-std=c++26 -freflection-latest -stdlib=libstdc++ --gcc-toolchain={GCC_TOOLCHAIN} -O1 -I."),
}
EXPECTED_UNAVAILABLE = {"clang-libstdcxx"}  # see the docstring


def lane_of(name: str) -> tuple[str, str]:
    """A lane name, or a raw compiler id with flags guessed from its family."""
    if name in LANES:
        return LANES[name]
    if name.startswith("clang"):
        return name, LANES["clang-libcxx"][1]
    if name.startswith("edg"):
        return name, "--c++26 -I."
    return name, LANES["gcc"][1]


def headers(example: pathlib.Path) -> list[tuple[str, str]]:
    """Every header the example needs, as (relative name, contents): es/**.hpp
    and any *.hpp next to main.cpp."""
    found = sorted((example / "es").rglob("*.hpp"))
    found += sorted(example.glob("*.hpp"))
    return [(str(p.relative_to(example)), p.read_text()) for p in found]


def main_source(example: pathlib.Path, project: str | None) -> str:
    text = (example / "main.cpp").read_text()
    return f"// Project: {project}\n{text}" if project else text


def request(example: pathlib.Path, compiler: str, flags: str, project: str | None = None) -> dict:
    files = [{"filename": name, "contents": text} for name, text in headers(example)]
    body = {
        "source": main_source(example, project),
        "options": {
            "userArguments": flags,
            "compilerOptions": {"executorRequest": True},
            "filters": {"execute": True},
            "tools": [],
            "libraries": [],
        },
        "lang": "c++",
        "files": files,
        "allowStoreCodeDebug": True,
    }
    data = json.dumps(body).encode()
    log(f"POST {API.format(compiler)}: main.cpp + {len(files)} headers, {len(data)} bytes, flags: {flags}")
    req = urllib.request.Request(
        API.format(compiler),
        data=data,
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            log(f"HTTP {r.status} after {time.monotonic() - t0:.1f}s; reading the reply")
            return json.load(r)
    except urllib.error.HTTPError as e:
        log(f"HTTP {e.code} {e.reason} after {time.monotonic() - t0:.1f}s: {e.read()[:300]!r}")
        raise
    except Exception as e:
        log(f"no reply after {time.monotonic() - t0:.1f}s: {e!r}")
        raise


def golden_layout(sessions: list[dict], tree: dict, compilers: list[dict], executors: list[dict],
                  layout: str) -> dict:
    """The same project as an explicit GoldenLayout config (what the site itself stores when you press Share),
    so the panes come up where a demo wants them. Component states follow lib/clientstate-normalizer.ts
    (ClientStateGoldenifier): codeEditor {id, source, lang, filename}, tree {id, …files…}, compiler {compiler,
    options, filters, libs, lang, tree}, executor {compiler, options, execArgs, execStdin, …Shown flags, tree,
    lang, wrap}. Sizes are percentages of the parent."""
    editors = [{"type": "component", "componentName": "codeEditor", "isClosable": True, "reorderEnabled": True,
                "title": session["filename"],
                "componentState": {"id": session["id"], "source": session["source"], "lang": "c++",
                                   "filename": session["filename"]}} for session in sessions]
    tree_pane = {"type": "component", "componentName": "tree", "isClosable": True, "reorderEnabled": True,
                 "componentState": {k: tree[k] for k in ("id", "cmakeArgs", "customOutputFilename",
                                                         "isCMakeProject", "compilerLanguageId", "files",
                                                         "newFileId")}}
    comp_panes = [{"type": "component", "componentName": "compiler", "isClosable": True, "reorderEnabled": True,
                   "componentState": {"compiler": c["id"], "options": c["options"], "filters": c["filters"],
                                      "libs": [], "lang": "c++", "tree": tree["id"], "flagsViewOpen": True}}
                  for c in compilers]
    exec_panes = [{"type": "component", "componentName": "executor", "isClosable": True, "reorderEnabled": True,
                   "componentState": {"compiler": e["compiler"]["id"], "options": e["compiler"]["options"],
                                      "execArgs": e["arguments"], "execStdin": e["stdin"],
                                      "compilationPanelShown": e["compilerVisible"],
                                      "compilerOutShown": e["compilerOutputVisible"],
                                      "argsPanelShown": e["argumentsVisible"],
                                      "stdinPanelShown": e["stdinVisible"], "tree": tree["id"], "libs": [],
                                      "lang": "c++", "wrap": e["wrap"]}} for e in executors]
    def stack(items, active=0, **size):
        return {"type": "stack", "isClosable": True, "reorderEnabled": True, "activeItemIndex": active,
                "content": items, **size}
    if layout == "tabs":  # the example-6 link: tree first, example.cpp shown; compiler then executor below
        content = [{"type": "column", "width": 100,
                    "content": [stack([tree_pane] + editors, active=1, height=60),
                                stack(comp_panes + exec_panes, active=0, height=40)]}]
    else:  # side
        content = [{"type": "row", "content": [
            {"type": "column", "width": 45, "content": [stack(editors, height=70), stack([tree_pane], height=30)]},
            {"type": "column", "width": 55, "content": [stack(comp_panes, height=50),
                                                        stack(exec_panes, height=50)]}]}]
    return {"version": 4, "content": content}


def asm_flags_of(flags: str, asm_flags: str | None) -> str:
    """The compiler pane's flags: the lane's language/feature flags (everything up to the first -O or -I) followed
    by `asm_flags`; None keeps the lane's flags."""
    if asm_flags is None:
        return flags
    keep = [f for f in flags.split() if not (f.startswith("-O") or f.startswith("-I") or f.startswith("-W"))]
    return " ".join(keep + asm_flags.split())


def short_link(example: pathlib.Path, lanes: list[str], project: str | None, layout: str = "tabs",
               open_files: list[str] | None = None, asm_flags: str | None = None) -> str:
    """Saves the example as one multi-file tree with a compiler + executor pane per lane; returns the short URL."""
    main_text = main_source(example, project)
    # Two rules of Compiler Explorer's saved state (lib/clientstate-normalizer.ts, static/multifile-service.ts):
    #  * the main source of a non-CMake C++ tree is found BY NAME: "example.cpp";
    #  * a tree file is shown in an editor only through a session: file.editorId = session.id, and the editor's
    #    text is session.source (fixFilesInTrees copies it into the file). The session has no compilers of its
    #    own: the tree's compiler/executor panes (one per lane) compile the project.
    session = {"id": 1, "language": "c++", "source": main_text, "conformanceview": False, "compilers": [],
               "executors": [], "filename": "example.cpp"}
    tree_files = [{"id": 1, "fileId": 1, "isIncluded": True, "isOpen": True, "isMainSource": True,
                   "filename": "example.cpp", "content": main_text, "editorId": session["id"],
                   "langId": "c++"}]
    extra = headers(example)
    if project:
        extra.append(("PROJECT.md", f"# {project}\n\nUploaded by ce-run.py from {example.name}/ "
                                    "(example.cpp is that directory's main.cpp).\n"))
    sessions = [session]
    wanted = list(open_files or [])
    for n, (name, text) in enumerate(extra, start=2):
        is_open = name in wanted
        if is_open:  # an editor tab needs a session of its own (editorId = session id)
            wanted.remove(name)
            sessions.append({"id": len(sessions) + 1, "language": "c++", "source": text,
                             "conformanceview": False, "compilers": [], "executors": [], "filename": name})
        tree_files.append({"id": n, "fileId": n, "isIncluded": name.endswith(".hpp"), "isOpen": is_open,
                           "isMainSource": False, "filename": name, "content": text,
                           "editorId": sessions[-1]["id"] if is_open else -1, "langId": "c++"})
    for name in wanted:
        print(f"--open {name}: no such file in the tree (names are as uploaded, e.g. es/serreg/reflect.hpp)")
    filters = {"binary": False, "binaryObject": False, "commentOnly": True, "demangle": True,
               "directives": True, "execute": False, "intel": True, "labels": True, "libraryCode": False,
               "trim": False, "debugCalls": False}
    compilers, executors = [], []
    for lane in lanes:
        compiler, flags = lane_of(lane)
        c = {"id": compiler, "options": flags, "filters": filters, "libs": [], "specialoutputs": [],
             "tools": [], "overrides": [], "tree": 1}
        compilers.append(dict(c, options=asm_flags_of(flags, asm_flags)))
        executors.append({"compilerVisible": False, "compilerOutputVisible": False, "arguments": "",
                          "argumentsVisible": True, "stdin": "", "stdinVisible": True,
                          "compiler": dict(c), "wrap": False, "runtimeTools": [], "overrides": []})
    tree = {"id": 1, "cmakeArgs": "", "customOutputFilename": "", "isCMakeProject": False,
            "buildSystem": "none", "compilerLanguageId": "c++", "files": tree_files,
            "newFileId": len(tree_files) + 1, "compilers": compilers, "executors": executors}
    state = ({"sessions": sessions, "trees": [tree]} if layout == "default"
             else golden_layout(sessions, tree, compilers, executors, layout))
    data = json.dumps(state).encode()
    log(f"POST {SHORTENER}: tree of {len(tree_files)} files, {len(data)} bytes, panes for lanes "
        f"{', '.join(lanes)}, layout {layout}")
    req = urllib.request.Request(SHORTENER, data=data,
                                 headers={"Content-Type": "application/json", "Accept": "application/json"})
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=120) as r:
        log(f"HTTP {r.status} after {time.monotonic() - t0:.1f}s")
        return json.load(r)["url"]


def lines(block) -> str:
    return "\n".join(x.get("text", "") for x in (block or []))


def main() -> int:
    ap = argparse.ArgumentParser(description="Build and run a ce-exampleN on Compiler Explorer.")
    ap.add_argument("--short-link", action="store_true", help="save one multi-file project with every lane")
    ap.add_argument("--project", metavar="NAME", help="project name (first-line comment + PROJECT.md)")
    ap.add_argument("--layout", choices=["tabs", "side", "default"], default="tabs",
                    help="pane layout of the saved project (see the docstring)")
    ap.add_argument("--open", action="append", default=[], metavar="FILE",
                    help="also open this tree file in an editor tab (repeatable)")
    ap.add_argument("--asm-flags", default="-O2 -Wall -Wextra -Wshadow -I.", metavar="F",
                    help="flags of the compiler (assembly) panes; the executors keep the lane's flags")
    ap.add_argument("--no-run", action="store_true", help="skip the compile+run requests (link only)")
    ap.add_argument("--verbose", "-v", action="store_true", help="print each step with a timestamp")
    ap.add_argument("--timeout", type=int, default=600, metavar="S", help="seconds per compile+run request")
    ap.add_argument("targets", nargs="*",
                    help="[example-dir] [lane | compiler-id ...]; lanes: " + ", ".join(LANES) +
                         " (default: the directory of this script, all lanes)")
    opts = ap.parse_args()
    global VERBOSE, TIMEOUT
    VERBOSE, TIMEOUT = opts.verbose, opts.timeout
    args = list(opts.targets)
    # the directory of the script as invoked (not resolved: each ce-exampleN/ce-run.py is a symlink to this file)
    example = pathlib.Path(__file__).absolute().parent
    if args and pathlib.Path(args[0]).is_dir():
        example = pathlib.Path(args.pop(0)).resolve()
    lanes = args or list(LANES)
    log(f"example {example}; lanes {', '.join(lanes)}; {len(headers(example))} headers found under es/ and ./")
    failed = 0
    for lane in ([] if opts.no_run else lanes):
        compiler, flags = lane_of(lane)
        print(f"===== {example.name} / lane {lane}: {compiler} {flags}", flush=True)
        try:
            r = request(example, compiler, flags, opts.project)
        except Exception as e:  # network, HTTP error, timeout
            print(f"request failed: {e}")
            failed += 1
            continue
        build = r.get("buildResult", {})
        if build.get("code", r.get("code", 0)) != 0:
            if lane in EXPECTED_UNAVAILABLE:
                print("unavailable (expected: see the docstring); first error:")
                print("\n".join(lines(build.get("stderr")).splitlines()[:3]))
                continue
            print("BUILD FAILED\n" + lines(build.get("stderr")) + lines(build.get("stdout")))
            failed += 1
            continue
        print(lines(r.get("stdout")))
        if r.get("stderr"):
            print(lines(r.get("stderr")))
        code = r.get("code")
        print(f"exit code {code}", flush=True)
        failed += code != 0
    if opts.short_link:
        try:
            print(f"short link ({example.name}, lanes {', '.join(lanes)}): {short_link(example, lanes, opts.project, opts.layout, opts.open, opts.asm_flags)}")
        except Exception as e:
            print(f"short link failed: {e}")
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
