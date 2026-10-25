#include "argv.h"
#include "config.h"
#include "generic_default.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct options {
    const char *profile; /* -p/--profile */
    const char *project; /* -P/--project */
    int yes;             /* -y/--yes */
    int dry_run;         /* -n/--dry-run */
    int list;            /* -l/--list */
};

static void usage(FILE *out)
{
    fputs("Usage: bubbleload [loader options] BINARY [args...]\n"
          "\n"
          "Run BINARY inside a bubblewrap sandbox using a per-program profile\n"
          "from $XDG_CONFIG_HOME/bubbleload/<NAME>.conf.\n"
          "\n"
          "Loader options (all of them must come before BINARY):\n"
          "  -p, --profile NAME   use profile NAME instead of the one derived\n"
          "                       from the basename of BINARY\n"
          "  -w, --rw DIR         extra directory, read-write (repeatable)\n"
          "  -r, --ro DIR         extra directory, read-only (repeatable)\n"
          "  -P, --project DIR    project directory (default: current directory)\n"
          "  -y, --yes            skip confirmation prompts\n"
          "  -n, --dry-run        print the bwrap command and exit 0\n"
          "  -l, --list           list available profiles and exit\n"
          "  -h, --help           show this help\n"
          "\n"
          "Everything after BINARY is passed to the program untouched; use \"--\"\n"
          "to stop loader option parsing.\n",
          out);
}

/* true when arg is exactly name, or name followed by '=value'. */
static int match_long(const char *arg, const char *name, const char **value)
{
    size_t n = strlen(name);

    if (strncmp(arg, name, n) != 0) {
        return 0;
    }
    if (arg[n] == '\0') {
        *value = NULL;
        return 1;
    }
    if (arg[n] == '=') {
        *value = arg + n + 1;
        return 1;
    }
    return 0;
}

static const char *take_value(int argc, char **argv, int *i, const char *opt,
                              const char *inline_value)
{
    if (inline_value != NULL) {
        if (inline_value[0] == '\0') {
            die("option %s requires a non-empty value", opt);
        }
        return inline_value;
    }
    if (*i + 1 >= argc) {
        die("option %s requires a value", opt);
    }
    *i += 1;
    return argv[*i];
}

/* ------------------------------------------------------------- prompting */

static char *prompt_line(const char *fmt, ...)
{
    va_list ap;
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    char *p;

    fflush(stdout);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    got = getline(&line, &cap, stdin);
    if (got < 0) {
        free(line);
        fputc('\n', stderr);
        return NULL;
    }
    while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r' ||
                       line[got - 1] == ' ' || line[got - 1] == '\t')) {
        line[--got] = '\0';
    }
    for (p = line; *p == ' ' || *p == '\t'; p++) {
        /* skip leading blanks */
    }
    if (p != line) {
        memmove(line, p, strlen(p) + 1u);
    }
    return line;
}

static void confirm_continue(int yes)
{
    if (yes) {
        return;
    }
    if (!isatty(STDIN_FILENO)) {
        die("stdin is not a TTY; use --yes to accept the fallback profile");
    }
    for (;;) {
        char *ans = prompt_line("Continue? [Y/n] ");

        if (ans == NULL) {
            die("aborted: no answer on stdin");
        }
        if (ans[0] == '\0' || strcmp(ans, "y") == 0 || strcmp(ans, "Y") == 0) {
            free(ans);
            return;
        }
        if (strcmp(ans, "n") == 0 || strcmp(ans, "N") == 0) {
            free(ans);
            die("aborted");
        }
        free(ans);
    }
}

/* ------------------------------------------------------------- profiles */

static char *config_dir(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");

    if (xdg != NULL && xdg[0] == '/') {
        return join_path(xdg, "bubbleload", "");
    }
    return join_path(home_dir(), ".config/bubbleload", "");
}

static char *conf_file(const char *dir, const char *name)
{
    return join_path(dir, name, ".conf");
}

static int file_exists(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        return 1;
    }
    if (errno == ENOENT) {
        return 0;
    }
    die("cannot access %s: %s", path, strerror(errno));
}

/* Write the built-in generic template to path.  With excl the file must not
 * exist yet (O_EXCL).  Returns 0, or -1 with errno set. */
static int write_template(const char *path, int excl)
{
    size_t len = strlen(bl_default_generic);
    size_t off = 0u;
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (excl ? O_EXCL : O_TRUNC);
    int fd;

    fd = open(path, flags, 0644);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0644) != 0) {
        int saved = errno;

        close(fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    while (off < len) {
        ssize_t written = write(fd, bl_default_generic + off, len - off);

        if (written < 0) {
            int saved;

            if (errno == EINTR) {
                continue;
            }
            saved = errno;
            close(fd);
            unlink(path);
            errno = saved;
            return -1;
        }
        off += (size_t)written;
    }
    if (close(fd) != 0) {
        return -1;
    }
    return 0;
}

/* Answer "y" to the regeneration prompt: create it, fail loudly on error. */
static void write_generic_profile(const char *dir)
{
    char *path = conf_file(dir, "generic");

    mkdir_p(dir, 0755);
    if (write_template(path, 0) != 0) {
        die("cannot write %s: %s", path, strerror(errno));
    }
    warnmsg("wrote built-in generic profile to %s", path);
    free(path);
}

/* First launch: the config directory does not exist at all, so create it and
 * seed generic.conf from the built-in template so that a fresh system works
 * without any post-install script.  This is best effort; if it fails the
 * regular fallback logic below reports a clear error.
 *
 * An existing directory is never touched, so deleting generic.conf on
 * purpose still gives the regeneration prompt instead of silently coming
 * back.  Only generic.conf is seeded: program specific profiles are examples,
 * not something a package should drop into a user's config directory. */
static void seed_config_dir(const char *dir)
{
    struct stat st;
    char *path;

    if (stat(dir, &st) == 0) {
        return;
    }
    if (errno != ENOENT) {
        return;
    }
    if (mkdir_p_try(dir, 0755) != 0) {
        return;
    }
    path = conf_file(dir, "generic");
    if (write_template(path, 1) == 0) {
        warnmsg("created %s from the built-in template", path);
    }
    free(path);
}

/* Find the profile file to load, running the generic-fallback protocol when
 * the requested profile does not exist.  name may be NULL when the derived
 * profile name is not a valid profile name. */
static char *select_profile_file(const char *dir, const char *name,
                                 const char *display, int yes)
{
    char *path;

    if (name != NULL) {
        path = conf_file(dir, name);
        if (file_exists(path)) {
            return path;
        }
        free(path);
    }

    if (name == NULL || strcmp(name, "generic") != 0) {
        warnmsg("no profile for '%s', falling back to 'generic'", display);
    }
    confirm_continue(yes);

    path = conf_file(dir, "generic");
    if (file_exists(path)) {
        return path;
    }

    if (!isatty(STDIN_FILENO)) {
        die("generic profile is missing: %s (stdin is not a TTY, cannot "
            "prompt; create it or run with --yes from a terminal)",
            path);
    }
    for (;;) {
        char *ans = prompt_line("generic profile is missing. Regenerate it "
                                "from the built-in default? [y/n/another] ");

        if (ans == NULL) {
            die("generic profile is missing and no answer was given on stdin");
        }
        if (strcmp(ans, "y") == 0 || strcmp(ans, "Y") == 0) {
            free(ans);
            free(path);
            write_generic_profile(dir);
            return conf_file(dir, "generic");
        }
        if (strcmp(ans, "n") == 0 || strcmp(ans, "N") == 0) {
            free(ans);
            die("no usable profile: %s is missing", path);
        }
        if (strcmp(ans, "another") == 0) {
            char *pname = prompt_line("Profile name: ");

            if (pname == NULL) {
                die("aborted: no answer on stdin");
            }
            if (!valid_name(pname)) {
                die("invalid profile name '%s'", pname);
            }
            free(path);
            path = conf_file(dir, pname);
            free(pname);
            if (!file_exists(path)) {
                die("no such profile: %s", path);
            }
            return path;
        }
        free(ans);
    }
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int list_profiles(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    struct strvec names;
    size_t i;

    strvec_init(&names);
    if (d == NULL) {
        if (errno == ENOENT) {
            fprintf(stderr, "bubbleload: no config directory: %s\n", dir);
            return 0;
        }
        die("opendir %s: %s", dir, strerror(errno));
    }
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        char *base;

        if (n <= 5u || strcmp(ent->d_name + n - 5u, ".conf") != 0) {
            continue;
        }
        base = xstrdup(ent->d_name);
        base[n - 5u] = '\0';
        if (!valid_name(base)) {
            free(base);
            continue;
        }
        strvec_push_owned(&names, base);
    }
    closedir(d);

    if (names.len > 0u) {
        qsort(names.v, names.len, sizeof *names.v, cmp_names);
    }
    for (i = 0; i < names.len; i++) {
        printf("%s\n", names.v[i]);
    }
    strvec_free(&names);
    if (fflush(stdout) != 0) {
        die("write error: %s", strerror(errno));
    }
    return 0;
}

/* --------------------------------------------------------------- output */

static void print_dry_run(const struct bl_emit *e)
{
    size_t g;

    for (g = 0; g < e->groups.len; g++) {
        size_t start = e->groups.v[g];
        size_t end = (g + 1u < e->groups.len) ? e->groups.v[g + 1u]
                                              : e->argv.len;
        size_t k;

        for (k = start; k < end; k++) {
            if (k != start) {
                fputc(' ', stdout);
            }
            fput_quoted(stdout, e->argv.v[k]);
        }
        fputc('\n', stdout);
    }
    if (fflush(stdout) != 0) {
        die("write error: %s", strerror(errno));
    }
}

/* ----------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    struct options opts;
    struct strvec raw_ro;
    struct strvec raw_rw;
    int i;
    int have_bin;
    int bin_index = 0;
    const char *binary;
    const char *home;
    char *home_real;
    char *pwd;
    char *project;
    char *dir;
    char *profile_name = NULL;
    const char *display_name;
    char *profile_path;
    struct strvec ro;
    struct strvec rw;
    struct bl_ctx ctx;
    struct bl_emit emit;

    memset(&opts, 0, sizeof opts);
    strvec_init(&raw_ro);
    strvec_init(&raw_rw);

    i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        const char *v = NULL;

        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[0] != '-' || a[1] == '\0') {
            break; /* first non-option: BINARY */
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(a, "-y") == 0 || strcmp(a, "--yes") == 0) {
            opts.yes = 1;
            continue;
        }
        if (strcmp(a, "-n") == 0 || strcmp(a, "--dry-run") == 0) {
            opts.dry_run = 1;
            continue;
        }
        if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) {
            opts.list = 1;
            continue;
        }
        if (strcmp(a, "-p") == 0 || match_long(a, "--profile", &v)) {
            opts.profile = take_value(argc, argv, &i, "-p/--profile", v);
            continue;
        }
        if (strcmp(a, "-w") == 0 || match_long(a, "--rw", &v)) {
            strvec_push(&raw_rw,
                        take_value(argc, argv, &i, "-w/--rw", v));
            continue;
        }
        if (strcmp(a, "-r") == 0 || match_long(a, "--ro", &v)) {
            strvec_push(&raw_ro,
                        take_value(argc, argv, &i, "-r/--ro", v));
            continue;
        }
        if (strcmp(a, "-P") == 0 || match_long(a, "--project", &v)) {
            opts.project =
                take_value(argc, argv, &i, "-P/--project", v);
            continue;
        }
        die("unknown option '%s' (try --help)", a);
    }
    have_bin = i < argc;

    dir = config_dir();
    if (opts.list) {
        int rc;

        seed_config_dir(dir);
        rc = list_profiles(dir);

        strvec_free(&raw_ro);
        strvec_free(&raw_rw);
        free(dir);
        return rc;
    }

    if (!have_bin) {
        usage(stderr);
        die("missing BINARY");
    }
    seed_config_dir(dir);
    binary = argv[i];
    bin_index = i;

    /* Resolve and validate paths. */
    home = home_dir();
    home_real = realpath(home, NULL);
    if (home_real == NULL) {
        home_real = xstrdup(home);
    }
    pwd = get_cwd();
    project = opts.project != NULL ? resolve_dir("-P/--project", opts.project)
                                   : xstrdup(pwd);

    strvec_init(&ro);
    strvec_init(&rw);
    {
        size_t k;

        for (k = 0; k < raw_ro.len; k++) {
            strvec_push_owned(&ro, resolve_dir("-r/--ro", raw_ro.v[k]));
        }
        for (k = 0; k < raw_rw.len; k++) {
            strvec_push_owned(&rw, resolve_dir("-w/--rw", raw_rw.v[k]));
        }
    }

    if (strcmp(project, home_real) == 0) {
        die("refusing to expose all of $HOME");
    }
    {
        size_t k;

        for (k = 0; k < rw.len; k++) {
            if (strcmp(rw.v[k], home_real) == 0) {
                die("refusing to expose all of $HOME");
            }
        }
    }

    /* Choose the profile. */
    display_name = path_basename(binary);
    if (opts.profile != NULL) {
        if (!valid_name(opts.profile)) {
            die("invalid profile name '%s' (allowed characters: "
                "[A-Za-z0-9._-], no leading dot)",
                opts.profile);
        }
        profile_name = xstrdup(opts.profile);
        display_name = opts.profile;
    } else if (valid_name(display_name)) {
        profile_name = xstrdup(display_name);
    }

    profile_path = select_profile_file(dir, profile_name, display_name,
                                       opts.yes);

    ctx.home = home;
    ctx.project = project;
    ctx.pwd = pwd;
    ctx.ro = &ro;
    ctx.rw = &rw;

    bl_emit_init(&emit);
    bl_load_profile(&emit, &ctx, dir, profile_path);

    if (!emit.has_exec && !find_in_path(binary)) {
        warnmsg("'%s' was not found in PATH; running it inside the sandbox "
                "may fail",
                binary);
    }

    /* Append the command and the user arguments. */
    bl_group(&emit);
    if (emit.has_exec) {
        size_t k;

        for (k = 0; k < emit.cmd.len; k++) {
            strvec_push(&emit.argv, emit.cmd.v[k]);
        }
    } else {
        strvec_push(&emit.argv, binary);
    }
    for (i = bin_index + 1; i < argc; i++) {
        strvec_push(&emit.argv, argv[i]);
    }

    if (opts.dry_run) {
        print_dry_run(&emit);
        bl_emit_free(&emit);
        strvec_free(&ro);
        strvec_free(&rw);
        strvec_free(&raw_ro);
        strvec_free(&raw_rw);
        free(home_real);
        free(pwd);
        free(project);
        free(dir);
        free(profile_name);
        free(profile_path);
        return 0;
    }

    /* Host side mkdir -p, so that later mounts do not fail. */
    {
        size_t k;

        for (k = 0; k < emit.mkdirs.len; k++) {
            mkdir_p(emit.mkdirs.v[k], 0777);
        }
    }

    if (!find_in_path("bwrap")) {
        die("bwrap was not found in PATH; install bubblewrap first");
    }
    strvec_terminate(&emit.argv);
    execvp("bwrap", emit.argv.v);
    die("failed to exec bwrap: %s", strerror(errno));
}
