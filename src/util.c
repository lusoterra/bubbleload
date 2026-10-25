#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void *must_alloc(void *p)
{
    if (p == NULL) {
        die("out of memory");
    }
    return p;
}

void die(const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    fputs("bubbleload: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void warnmsg(const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    fputs("bubbleload: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void *xmalloc(size_t n)
{
    return must_alloc(malloc(n == 0u ? 1u : n));
}

void *xzalloc(size_t n)
{
    return must_alloc(calloc(1u, n == 0u ? 1u : n));
}

void *xrealloc(void *p, size_t n)
{
    return must_alloc(realloc(p, n == 0u ? 1u : n));
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *d = xmalloc(n);

    memcpy(d, s, n);
    return d;
}

char *xasprintf(const char *fmt, ...)
{
    size_t cap = 128u;

    for (;;) {
        va_list ap;
        char *buf;
        int n;

        buf = xmalloc(cap);
        va_start(ap, fmt);
        n = vsnprintf(buf, cap, fmt, ap);
        va_end(ap);
        if (n < 0) {
            free(buf);
            die("formatting error");
        }
        if ((size_t)n < cap) {
            return buf;
        }
        free(buf);
        cap = (size_t)n + 1u;
    }
}

static int shell_safe_char(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }
    return strchr("_@%+=:,./-", (int)c) != NULL;
}

void fput_quoted(FILE *out, const char *s)
{
    const char *p;
    int safe = 1;

    if (*s == '\0') {
        fputs("''", out);
        return;
    }
    for (p = s; *p != '\0'; p++) {
        if (!shell_safe_char((unsigned char)*p)) {
            safe = 0;
            break;
        }
    }
    if (safe) {
        fputs(s, out);
        return;
    }
    fputc('\'', out);
    for (p = s; *p != '\0'; p++) {
        if (*p == '\'') {
            fputs("'\\''", out);
        } else {
            fputc(*p, out);
        }
    }
    fputc('\'', out);
}

const char *home_dir(void)
{
    static char *cached = NULL;

    if (cached == NULL) {
        const char *h = getenv("HOME");

        if (h == NULL || h[0] == '\0') {
            die("$HOME is not set");
        }
        if (h[0] != '/') {
            die("$HOME is not an absolute path: %s", h);
        }
        cached = xstrdup(h);
    }
    return cached;
}

char *resolve_dir(const char *what, const char *path)
{
    char *real = realpath(path, NULL);
    struct stat st;

    if (real == NULL) {
        die("%s: %s: %s", what, path, strerror(errno));
    }
    if (stat(real, &st) != 0) {
        die("%s: %s: %s", what, path, strerror(errno));
    }
    if (!S_ISDIR(st.st_mode)) {
        die("%s: not a directory: %s", what, path);
    }
    return real;
}

char *get_cwd(void)
{
    size_t size = 256u;

    for (;;) {
        char *buf = xmalloc(size);

        if (getcwd(buf, size) != NULL) {
            return buf;
        }
        free(buf);
        if (errno != ERANGE) {
            die("getcwd: %s", strerror(errno));
        }
        if (size > ((size_t)-1) / 2u) {
            die("current directory path is too long");
        }
        size *= 2u;
    }
}

char *join_path(const char *dir, const char *name, const char *suffix)
{
    size_t dl = strlen(dir);

    if (dl > 0u && dir[dl - 1u] == '/') {
        return xasprintf("%s%s%s", dir, name, suffix);
    }
    return xasprintf("%s/%s%s", dir, name, suffix);
}

const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash != NULL ? slash + 1 : path;
}

int find_in_path(const char *name)
{
    const char *path;
    struct stat st;

    if (name[0] == '\0') {
        return 0;
    }
    if (strchr(name, '/') != NULL) {
        return stat(name, &st) == 0 && S_ISREG(st.st_mode) &&
               access(name, X_OK) == 0;
    }

    path = getenv("PATH");
    if (path == NULL) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }

    for (;;) {
        const char *end = strchr(path, ':');
        size_t len = end != NULL ? (size_t)(end - path) : strlen(path);
        char *cand;
        int found;

        if (len == 0u) {
            cand = xasprintf("./%s", name);
        } else {
            cand = xasprintf("%.*s/%s", (int)len, path, name);
        }
        found = stat(cand, &st) == 0 && S_ISREG(st.st_mode) &&
                access(cand, X_OK) == 0;
        free(cand);
        if (found) {
            return 1;
        }
        if (end == NULL) {
            break;
        }
        path = end + 1;
    }
    return 0;
}

static void make_one_dir(const char *path, mode_t mode)
{
    struct stat st;

    if (mkdir(path, mode) == 0) {
        return;
    }
    if (errno != EEXIST) {
        die("mkdir %s: %s", path, strerror(errno));
    }
    if (stat(path, &st) != 0) {
        die("mkdir %s: %s", path, strerror(errno));
    }
    if (!S_ISDIR(st.st_mode)) {
        die("%s exists but is not a directory", path);
    }
}

void mkdir_p(const char *path, mode_t mode)
{
    char *copy = xstrdup(path);
    char *p;

    for (p = copy + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        make_one_dir(copy, mode);
        *p = '/';
    }
    make_one_dir(copy, mode);
    free(copy);
}

void check_secure_file(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        die("%s: %s", path, strerror(errno));
    }
    if (!S_ISREG(st.st_mode)) {
        die("%s: not a regular file", path);
    }
    if (st.st_uid != getuid()) {
        die("%s: not owned by uid %ld, refusing to read it", path,
            (long)getuid());
    }
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        die("%s: group or world writable (mode %04o), refusing to read it",
            path, (unsigned)(st.st_mode & 07777u));
    }
}

int valid_name(const char *name)
{
    const char *p;

    if (name == NULL || name[0] == '\0' || name[0] == '.') {
        return 0;
    }
    for (p = name; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            continue;
        }
        return 0;
    }
    return 1;
}
