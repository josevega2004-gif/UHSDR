#!/usr/bin/env python3
"""Compile the firmware's translation units once per build option that is switched off, and say
which of them no longer compile.

    make build-config-check

The build matrix in .github/workflows/build.yml varies the *board* flags - which UI, which RF
board, which MCU - and nothing at all varies the audio and debug options that
hardware/uhsdr_board_config.h and hardware/uhsdr_board.h leave commented out.  Those lines are
claims: "uncomment this and the mode comes back".  Two of them had not been true for years, and no
check could see it, because the only build that ever ran had the option off.

What this does, for one board configuration:

  * discovers every value-less `#define` that those two headers leave commented out;
  * asks the build system itself for the command line of every translation unit of `make all`;
  * runs each one with `-fsyntax-only`, once with no option added (that sweep must be clean, or the
    toolchain or the configuration is broken and the rest of the numbers would mean nothing), then
    once per option with `-D<OPTION>` added;
  * compares the set of files that fail per option with the manifest in
    support/build_config_known_breakage.txt.

Both directions are errors.  A file that starts failing is a regression against an option somebody
can still switch on.  A file that stops failing is a manifest line that has to be deleted, which is
how the manifest stays a report rather than becoming a list of things nobody remembers: fixing the
last entry for an option is the point at which that option becomes buildable, and it is worth saying
out loud in a commit.

`-fsyntax-only` is deliberately the whole check: it parses and type checks, which is where a mode
that drifted from the API fails.  It does not link, so an option that compiles every file but calls a
function nobody defines is not caught here, and no object code is produced, which is what keeps a
full sweep of every option to a couple of minutes rather than to a second firmware build.
"""

import argparse
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys
import tempfile

# the headers whose commented-out defines are the options users are invited to switch on
OPTION_HEADERS = ("hardware/uhsdr_board_config.h", "hardware/uhsdr_board.h")
MANIFEST = "support/build_config_known_breakage.txt"
DEFAULT_CONFIGFLAGS = "-DUI_BRD_MCHF -DRF_BRD_MCHF"

# a commented-out `#define NAME` with nothing after it: a switch.  One with a value after it is a
# setting (EXPFLAGS1_RESERVE_8 0x100, DISPLAY_S_METER_STD 0) and turning it on without the value the
# code expects is not a configuration anybody can choose from the header anyway.
OPTION_RE = re.compile(r"^\s*//\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")
# the compiler line for one source file, as make -n prints it.  The object file comes after the
# source here, so the source is found by its extension rather than by its position.
GCC = "arm-none-eabi-gcc"


def discover_options(root):
    """Every option the config headers leave off, with the header and line it is written on.

    The pair is what the report needs: an option can be commented out in two headers at once, and the
    one that matters for a fix is wherever the maintainer left the note about it.
    """
    found = {}
    for rel in OPTION_HEADERS:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue
        with open(path, encoding = "utf-8", errors = "replace") as handle:
            for number, line in enumerate(handle, 1):
                match = OPTION_RE.match(line)
                if match:
                    found.setdefault(match.group(1), (rel, number))
    return found


def _strip_output_flags(argv):
    """The command line for one file, kept as the build wrote it except for what -fsyntax-only
    makes moot: no dependency file, no object file."""
    out, skip = [], 0
    for arg in argv:
        if skip:
            skip = 0
            continue
        if arg in ("-MT", "-MF", "-o"):
            skip = 1
            continue
        if arg in ("-MMD", "-MP"):
            continue
        out.append(arg)
    return out


def compile_commands(root, build_dir, configflags, makeargs, target):
    """Ask the build system for the command line of every translation unit of `target`.

    Rebuilding these lines here would be a second copy of the include paths and -D list, and the two
    copies would disagree the first time someone adds an option to the Makefile - which is the exact
    fault this script exists to catch in the firmware.  `make -n` costs seconds and compiles nothing.
    """
    cmd = ["make", "-n", "-f", os.path.join(root, "Makefile"), "ROOTLOC=" + root,
           "CONFIGFLAGS=" + configflags] + makeargs + [target]
    with open(os.path.join(build_dir, "make-dry-run.log"), "w") as log:
        done = subprocess.run(cmd, cwd = build_dir, stdout = subprocess.PIPE,
                              stderr = subprocess.STDOUT, text = True)
        log.write(done.stdout)
    if done.returncode != 0:
        raise SystemExit("build-config: %s exited %d writing the dry run, see %s"
                         % (cmd[0] + " -n", done.returncode, os.path.join(build_dir, "make-dry-run.log")))
    units = []
    for line in done.stdout.splitlines():
        if GCC not in line:
            continue
        try:
            argv = shlex.split(line)
        except ValueError:
            continue
        if not argv or not argv[0].endswith(GCC):
            continue
        sources = [a for a in argv[1:] if a.endswith(".c")]
        if len(sources) != 1:            # a link line, an assembly unit, or a shape this script
            continue                      # should not guess at
        src = sources[0]
        argv = _strip_output_flags(argv)
        rel = os.path.relpath(src, root) if os.path.isabs(src) else src
        # the compiler as the build named it, the flags, then -fsyntax-only in place of writing an
        # object: the flags are the build's own, which is the whole point of coming through make
        units.append((rel, argv[0], argv[1:] + ["-fsyntax-only", src]))
    return units


def sweep(units, define, jobs, deps = None):
    """Compile every unit with -D<define> (or bare), returning {file: first errors} for the failures.

    With a `deps` mapping each unit is also asked to write its include file (`-MD`), which is how the
    per-option sweeps work out which units can even see the option.  That costs a fraction of a
    compile and is done once, on the baseline sweep.
    """
    extra = ["-D" + define] if define else []

    def one(item):
        rel, compiler, argv = item
        args = extra + (["-MD", "-MF", deps[rel]] if deps is not None else [])
        done = subprocess.run([compiler] + args + argv, stdout = subprocess.DEVNULL,
                              stderr = subprocess.PIPE, text = True)
        if done.returncode == 0:
            return None
        text = done.stderr or "(compiler produced no message)"
        # in order, without repeats: a #error or a missing prototype is often reported twice for one
        # line, and three copies of it would crowd out the other two messages worth reading
        first, seen = [], set()
        for line in text.splitlines():
            if "error:" in line and line.strip() not in seen:
                seen.add(line.strip())
                first.append(line.strip())
            if len(first) == 3:
                break
        if not first:
            first = [text.splitlines()[0][:200]]
        return rel, "\n".join(first)

    failed = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers = jobs) as pool:
        for result in pool.map(one, units):
            if result:
                failed[result[0]] = result[1]
    return failed


_OPTION_WORD = {}


def mentions_option(root, path, option, cache):
    """Whether one file names this option, ignoring the commented-out `#define` that offers it.

    The offer must not count as a mention, or every file including hardware/uhsdr_board_config.h -
    which is nearly all of them - would look affected by every option and no sweep would ever be
    smaller than the whole tree.  A file that cannot be read counts as mentioning it, so an
    unreadable include widens the sweep rather than silently skipping work.
    """
    key = (path, option)
    if key in cache:
        return cache[key]
    # whole word, because an option can be the prefix of another (USE_GFX_SSD1289 next to a
    # hypothetical USE_GFX_SSD1289_EXTRA) and the shorter one must not be found in the longer name
    word = _OPTION_WORD.setdefault(option, re.compile(r"\b" + re.escape(option) + r"\b"))
    found = False
    try:
        with open(path if os.path.isabs(path) else os.path.join(root, path),
                  encoding = "utf-8", errors = "replace") as handle:
            for line in handle:
                if OPTION_RE.match(line) and option in line:
                    continue                       # this is the offer, not a use
                if word.search(line):
                    found = True
                    break
    except OSError:
        found = True
    cache[key] = found
    return found


def visible_units(units, deps, root, option, cache):
    """The units whose own file, or some file they include, names the option.

    A unit outside this set preprocesses the same with and without -D<option>, so compiling it twice
    would only spend the time: the option cannot reach it.  The exception worth naming is an option
    assembled by token pasting, which no textual search sees - none of the options these headers offer
    is built that way - and --full sweeps the tree regardless.
    """
    out = []
    for rel, compiler, argv in units:
        files = deps.get(rel)
        if files is None:            # no dependency file for this unit: sweep it rather than guess
            out.append((rel, compiler, argv))
            continue
        if any(mentions_option(root, f, option, cache) for f in (files | {rel})):
            out.append((rel, compiler, argv))
    return out


def read_manifest(root):
    """{option: {file, ...}} plus the options the manifest says not to sweep at all.

    `SKIP option` is for an option that cannot be switched on by itself - one that needs a matching
    -I, or a second option, or hardware that is not in the build.  It is stated rather than silently
    dropped so the list of what this check does not look at is in the same file as what it does.
    """
    path = os.path.join(root, MANIFEST)
    expected, skip = {}, set()
    if not os.path.isfile(path):
        raise SystemExit("build-config: no manifest at %s. If every option compiles, write the file "
                         "with just its comments; otherwise the check has nothing to compare against."
                         % path)
    with open(path, encoding = "utf-8") as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if parts[0] == "SKIP":
                skip.add(parts[1])
            elif len(parts) == 2:
                expected.setdefault(parts[0], set()).add(parts[1])
            else:
                raise SystemExit("build-config: manifest line is neither OPTION file nor SKIP OPTION: "
                                 + line)
    return expected, skip


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description = __doc__.splitlines()[0])
    ap.add_argument("--configflags", default = DEFAULT_CONFIGFLAGS,
                    help = "board flags for the configuration to sweep (default: %(default)s)")
    ap.add_argument("--target", default = "all", help = "make target whose objects to sweep")
    ap.add_argument("--makearg", action = "append", default = [], metavar = "VAR=value",
                    help = "extra variable for the dry run, repeatable (BUILDFOR=F7, TRX_ID=ovi40, "
                           "OPT_GCC_ARM=/path/to/toolchain, ...)")
    ap.add_argument("--option", action = "append", default = [], metavar = "NAME",
                    help = "sweep only this option, repeatable; the no-option baseline is still checked")
    ap.add_argument("--quick", action = "store_true",
                    help = "skip the sweep of the shipped configuration, for iterating on one option "
                           "when that baseline is already known to be clean; without it every unit is "
                           "swept for every option, since the include lists come from the baseline")
    ap.add_argument("--full", action = "store_true",
                    help = "sweep every translation unit for every option rather than only the units "
                           "whose includes name it; use it when regenerating the manifest")
    ap.add_argument("--list", action = "store_true", help = "print the discovered options and stop")
    ap.add_argument("--jobs", type = int, default = os.cpu_count() or 4)
    ap.add_argument("--keep-dry-run", action = "store_true",
                    help = "leave the build dir with the make dry run in it")
    args = ap.parse_args()

    if subprocess.run(["which", "arm-none-eabi-gcc"], stdout = subprocess.DEVNULL).returncode:
        print("build-config: FAIL arm-none-eabi-gcc is not on PATH. Pass the toolchain the same way "
              "the build job does (--makearg OPT_GCC_ARM=/path/to/gcc-arm-none-eabi) and put its bin "
              "directory on PATH, since the command lines come from make unchanged.")
        return 1

    options = discover_options(root)
    wanted = [o for o in sorted(options) if not args.option or o in args.option]
    if args.list:
        for opt in wanted:
            print("%-36s commented out at %s:%d" % (opt, options[opt][0], options[opt][1]))
        return 0
    if not wanted:
        print("build-config: FAIL no commented-out options found in %s" % ", ".join(OPTION_HEADERS))
        return 1

    build_dir = tempfile.mkdtemp(prefix = "build-config-")
    try:
        units = compile_commands(root, build_dir, args.configflags, args.makearg, args.target)
        if not units:
            print("build-config: FAIL the dry run named no %s translation units, so the build system "
                  "changed shape; see %s" % (args.target, os.path.join(build_dir, "make-dry-run.log")))
            return 1
        print("build-config: %d translation units of `%s %s`, %d option(s) offered by the config headers"
              % (len(units), args.configflags, args.target, len(wanted)))

        dep_dir = os.path.join(build_dir, "deps")
        os.makedirs(dep_dir, exist_ok = True)
        dep_files = {rel: os.path.join(dep_dir, re.sub(r"[^A-Za-z0-9]", "_", rel) + ".d")
                     for rel, _c, _a in units}
        baseline = {} if args.quick else sweep(units, None, args.jobs, dep_files)
        if args.quick:
            print("build-config: --quick, the shipped configuration is assumed to compile, and units "
                  "are not filtered by their includes (the dependency files come from that sweep)")
        deps = {}
        for rel, path in dep_files.items():
            try:
                with open(path, encoding = "utf-8", errors = "replace") as handle:
                    body = handle.read().replace("\\\n", " ")
                names = set(body.split(":", 1)[1].split())
            except (OSError, IndexError):
                # no dep file (there is none with --quick, and the baseline writes one per unit
                # otherwise): visible_units() sweeps such a unit unconditionally rather than guess
                deps[rel] = None
                continue
            deps[rel] = {os.path.relpath(n, root) if os.path.isabs(n) else n for n in names}
        if baseline:
            print("build-config: FAIL the configuration as shipped does not compile, so a per-option "
                  "sweep would measure nothing. %d file(s) fail, starting with:" % len(baseline))
            for name in sorted(baseline)[:3]:
                print("  %s\n    %s" % (name, baseline[name].replace("\n", "\n    ")))
            return 1

        expected, skip = read_manifest(root)
        cache = {}
        problems = []
        for opt in wanted:
            if opt in skip:
                print("  %-36s skipped, and the manifest says why" % opt)
                continue
            seen = units if args.full else visible_units(units, deps, root, opt, cache)
            note = "" if args.full else ", %d of %d units can see it" % (len(seen), len(units))
            failed = sweep(seen, opt, args.jobs) if seen else {}
            want = expected.get(opt, set())
            fresh, healed = sorted(set(failed) - want), sorted(want - set(failed))
            if not fresh and not healed:
                print("  %-36s %d file(s) fail, as the manifest recorded%s" % (opt, len(failed), note))
                continue
            print("  %-36s %d failing, %d new, %d fixed%s" % (opt, len(failed), len(fresh),
                                                              len(healed), note))
            for name in fresh:
                print("    NEW  %s\n           %s" % (name, failed[name].replace("\n", "\n           ")))
                problems.append("%s: %s no longer compiles" % (opt, name))
            for name in healed:
                print("    FIXED %s compiles again under -D%s: delete that line from %s"
                      % (name, opt, MANIFEST))
                problems.append("%s: %s compiles again, the manifest still lists it" % (opt, name))
        for opt in sorted(expected):
            if opt not in options and opt not in skip:
                print("  %-36s manifest names files for an option the headers no longer offer" % opt)
                problems.append("%s: the manifest is about an option that is not switched off anywhere" % opt)

        if problems:
            print("build-config: FAIL %d difference(s) from %s" % (len(problems), MANIFEST))
            for line in problems:
                print("  " + line)
            print("build-config: an option that broke is a mode somebody can still switch on and "
                  "cannot build; an option listed as broken that now compiles is worth its manifest "
                  "line deleted, and the mode is then buildable.")
            return 1
        print("build-config: ok - the shipped configuration compiles, and every option that does not "
              "fails exactly the files %s records" % MANIFEST)
        return 0
    finally:
        if args.keep_dry_run:
            print("build-config: dry run left in %s" % build_dir)
        else:
            subprocess.run(["rm", "-rf", build_dir])


if __name__ == "__main__":
    sys.exit(main())
