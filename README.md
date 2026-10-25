# bubbleload

`bubbleload` launches a program inside a [bubblewrap](https://github.com/containers/bubblewrap)
(`bwrap`) sandbox, driven by a small per-program config file.  It is a tiny
native C11 program (POSIX + libc only, no third-party dependencies) that turns
a declarative profile into a `bwrap` argv array and `execvp()`s it, so no
wrapper process stays alive.

```
bubbleload [loader options] BINARY [args...]
```

```
bubbleload --rw ../project-pkgbuild opencode -s session_id
```

Everything after `BINARY` is passed to the program untouched.

## Build, test, install

```sh
make            # build/bubbleload
make test       # runs tests/run.sh against the normal and the ASan/UBSan build
make install    # PREFIX=/usr/local, DESTDIR supported
make install-profiles   # optional, source installs only: seed ~/.config/bubbleload
make clean
```

`make install` writes only under `$(DESTDIR)$(PREFIX)`: the binary and the
example profiles in `share/bubbleload/examples`.  It never touches a home
directory, so packaging needs no post-install script.  Profiles live in
`$XDG_CONFIG_HOME/bubbleload` (falling back to `~/.config/bubbleload`) and the
binary creates that directory itself on first launch, seeding `generic.conf`
from its built-in copy (see below).

The build is `-std=c11 -Wall -Wextra -Wpedantic -Werror` with
`-O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE -pie`; the test target
additionally builds with `-fsanitize=address,undefined` and runs the whole
suite under it.  `bwrap` is only needed at run time, not for the tests (they
use `--dry-run`), except for the optional smoke test.

The sanitizer run disables LeakSanitizer (`detect_leaks=0`) because the error
paths intentionally `exit(1)` with a few allocations still live; the successful
code paths are leak-free.

## Loader options

Loader options are parsed only *before* `BINARY`; the first non-option
argument (or `--`) ends loader option parsing.

| Option | Meaning |
| --- | --- |
| `-p`, `--profile NAME` | profile to use instead of the one derived from `BINARY` |
| `-w`, `--rw DIR` | extra directory, read-write (repeatable) |
| `-r`, `--ro DIR` | extra directory, read-only (repeatable) |
| `-P`, `--project DIR` | project directory (default: current directory) |
| `-y`, `--yes` | skip confirmation prompts |
| `-n`, `--dry-run` | print the final bwrap command (shell-quoted, one flag group per line) and exit 0 |
| `-l`, `--list` | list the profiles in the config directory and exit |
| `-h`, `--help` | show help |

`-w`/`-r`/`-P`/`-p` also accept the `--long=value` form.  Paths are resolved
with `realpath()` and must exist (and be directories).  The run is refused if
the project directory or any `--rw` directory is `$HOME`
(`refusing to expose all of $HOME`).

## Profiles

The config directory is `$XDG_CONFIG_HOME/bubbleload`, or
`~/.config/bubbleload` when `XDG_CONFIG_HOME` is unset (a relative
`XDG_CONFIG_HOME` is ignored, as the XDG spec requires).  The profile is
`<config dir>/<NAME>.conf`, where `NAME` is `--profile NAME` if given, else the
basename of `BINARY`.

`NAME` must match `[A-Za-z0-9._-]+` and must not start with a dot, so no
profile can escape the config directory.  Config files must be owned by the
current user and must not be group/world writable; otherwise `bubbleload`
refuses to read them.  The same checks apply to `include`d files.

### Generic fallback

If the config directory does not exist at all, it is created (mode 0755) and
`generic.conf` is seeded from the built-in template (mode 0644) before
anything else happens, so a fresh install works without a post-install
script:

```
bubbleload: created /home/you/.config/bubbleload/generic.conf from the built-in template
```

This only ever happens when the whole directory is missing, and it only seeds
`generic.conf`; program specific profiles are examples that a package should
not drop into a user's config directory.  If the directory exists but
`generic.conf` was deleted, it is *not* recreated behind your back — the
prompt below runs instead.  Seeding is best effort: if the location is not
writable, the normal error is reported.

If the profile file does not exist, the special profile `generic`
(`generic.conf`) is used instead:

1. A warning is printed:
   `bubbleload: no profile for '<NAME>', falling back to 'generic'`, followed
   by `Continue? [Y/n]` on a terminal.  With `--yes` the question is skipped.
2. If `generic.conf` does not exist either, the prompt
   `generic profile is missing. Regenerate it from the built-in default? [y/n/another]`
   is shown:
   * `y` writes the built-in template to `generic.conf` (mode 0644) and
     continues,
   * `n` exits 1,
   * `another` asks for a profile name, validates it, requires the file to
     exist and uses that profile instead.
3. Without a TTY nothing is ever prompted: the problem is reported and
   `bubbleload` exits 1.  `--yes` only answers step 1; step 2 always needs a
   terminal.

## Config format

Line based, hand parsed.  `#` at the start of a token begins a comment, blank
lines are ignored.  Tokens are separated by whitespace; double quotes group a
token and a backslash inside double quotes escapes the next character (so
`"a\"b"` is `a"b`).  Outside double quotes a backslash is a literal
character.  Every token is then expanded:

* `$HOME`, `$PROJECT`, `$PWD`, `$XDG_RUNTIME_DIR`, `$UID` — `${VAR}` is also
  accepted.  Unknown variables, and known variables that are not set, are
  errors.
* a leading `~` (followed by `/` or end of token) means `$HOME`; `~user` is
  rejected.

Errors are reported as `file:line: message`.  Directives are emitted in file
order (bwrap layers mounts in order, later ones win); `include` is inlined at
the include position.

| Directive | Effect |
| --- | --- |
| `include NAME` | load `<config dir>/NAME.conf` here (max 8 nested includes, cycles detected) |
| `exec CMD [ARGS...]` | command to run inside the sandbox; program args are appended after it.  Without `exec`, `BINARY` itself is run |
| `die-with-parent`, `unshare-all`, `share-net`, `new-session` | matching bwrap flags |
| `hostname NAME` | `--hostname NAME` |
| `ro-bind SRC [DST]`, `ro-bind-try`, `bind`, `bind-try` | matching bwrap flags, `DST` defaults to `SRC` |
| `symlink TARGET LINK` | `--symlink TARGET LINK` |
| `dir`, `tmpfs`, `proc`, `dev`, `chdir` `PATH` | matching bwrap flags |
| `setenv NAME VALUE`, `unsetenv NAME` | matching bwrap flags |
| `mask PATH` | `--ro-bind /dev/null PATH` |
| `mkdir PATH` | **host side**: `mkdir -p PATH` before launching, so later binds do not fail |
| `resolv-conf` | if `realpath(/etc/resolv.conf)` is outside `/etc`, add `--ro-bind` of it to itself (systemd-resolved setups) |
| `project` | `--bind $PROJECT $PROJECT --chdir $PROJECT` |
| `extra-binds` | the `--ro`/`--rw` binds from the command line, at this position |
| `raw ARG...` | tokens passed verbatim to bwrap (escape hatch) |

If `extra-binds` is absent, the command line binds are emitted just before
`project`, or at the end of the argument list when the profile has no
`project` directive.  Read-only binds come before read-write ones, like the
original script.

The built-in generic template is embedded in the binary; it is generated at
build time from `examples/generic.conf` (`tools/embed.sh`), so the shipped
example and the built-in default cannot drift apart.

## Examples

`examples/opencode.conf` is a faithful translation of the original
`bwrap-opencode` shell script (kept as a reference in the tests);
`examples/generic.conf` is the conservative default: everything unshared, no
network, read-only `/usr` and `/etc`, a private empty `$HOME`, the project
directory writable, no display variables.  Uncomment `share-net` in the
profile to allow network access.

Copy a profile to `~/.config/bubbleload/<name>.conf` (or use `-p`) and run
`bubbleload <name> ...`.  `-n` shows exactly what would be executed.

## Tests

`tests/run.sh` (run by `make test`) only uses `--dry-run`, so it needs no
sandbox.  It covers option parsing before/after `BINARY`, `--`, `-p`
overrides, includes and cycle/depth detection, variable expansion errors,
profile name validation, `$HOME` refusal, the generic fallback (including the
non-TTY refusal), first-launch seeding (directory creation, mode 0644,
identical to the shipped example, never overwriting an existing directory),
`extra-binds` placement, byte-for-byte argument passthrough, and a
word-for-word comparison of `opencode.conf` with the argument list the
original bash script would build.  One optional smoke test
runs `bubbleload -p generic sh -c 'echo ok'` and is skipped when `bwrap` is
missing.

## Design decisions and notes

* **Project directory.**  This repository *is* the `./bubbleload` directory
  from the task; `src/`, `examples/`, `tests/`, `Makefile` and `README.md`
  live at its root.
* **No shell anywhere.**  The bwrap argv is built as an array and `execvp()`ed
  directly.  The vector is explicitly NUL-terminated (a missing terminator
  made `execvp` fail with `EFAULT` — the sanitizer build in `make test` caught
  it).
* **`exec` vs `BINARY`.**  `BINARY` selects the profile and is only the
  command when the profile has no `exec` directive.  Program arguments are
  always appended to whatever the profile's `exec` line says.
* **Exit codes.**  Everything before `exec` either succeeds or exits 1; there
  is no unsandboxed fallback.  `--dry-run`, `--list` and `--help` exit 0.
* **First-launch bootstrap.**  The config directory is created and
  `generic.conf` seeded from the embedded template only when the directory
  itself is absent, never when it exists with files missing.  The write is
  best effort (a read-only `$HOME` does not produce a crash, just the normal
  "generic profile is missing" error), uses `O_EXCL` so a concurrent run
  cannot clobber a file, and is announced on stderr.  This is a deliberate
  deviation from the "always prompt when generic.conf is missing" rule in the
  original spec, limited to the true first launch.
* **No unsandboxed fallback, no post-install script.**  `make install` only
  writes under `$(DESTDIR)$(PREFIX)`; the lazy seeding above is what makes a
  package work on a machine where `bubbleload` has never run.
* **`-l` on a missing config directory** seeds it first (so a fresh system
  lists `generic`); if it still does not exist it prints a note on stderr and
  exits 0 (an empty list is not an error).
* **`--dry-run`** has no side effects: it does not create `mkdir` directories
  and does not require `bwrap` to be installed.  It still prints the
  "not found in PATH" warning for `BINARY`.
* **`bwrap` missing** is only diagnosed when actually launching, with
  `bwrap was not found in PATH; install bubblewrap first`.
* **Variable expansion details.**  `~` is expanded before `$VAR`, so
  `"~/$XDG_RUNTIME_DIR"`-style tokens work; `$PWD` is the physical working
  directory (`getcwd()`), which can differ from a shell's logical `$PWD`;
  `$HOME` is used as-is for expansion and `realpath()`ed only for the
  "expose all of `$HOME`" comparison.  A known but unset variable
  (`$XDG_RUNTIME_DIR`) is a hard error rather than an empty string.
* **`#` comments** only start at the beginning of a token, so `foo#bar` is a
  literal argument.
* **Include depth** is counted from the top-level profile: up to 8 nested
  includes are allowed; cycles are detected on `realpath()`-canonicalized
  paths.
* **Repeated directives.**  The last `exec` wins.  Every `extra-binds` in a
  profile is emitted at its own position (repeating it duplicates the binds,
  which bwrap tolerates).
* **`resolv-conf`** emits nothing when `/etc/resolv.conf` cannot be resolved
  (unusual container setups) instead of failing.
* **Profile name derivation.**  If `--profile` is invalid, that is an error.
  If the *derived* basename is not a valid profile name (e.g. a binary called
  `.foo`), `bubbleload` treats it like a missing profile and uses the generic
  fallback.  When the requested name is already `generic`, the redundant
  "no profile for 'generic'" warning is skipped.
* **`--long=value`** is accepted for all options that take a value.
* **Difference from the old script.**  The project directory is `-P/--project`
  instead of a positional argument, because everything after `BINARY` belongs
  to the program.  Behaviour of the emitted bwrap argument list is otherwise
  identical (verified by a test).
