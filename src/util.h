#ifndef BUBBLELOAD_UTIL_H
#define BUBBLELOAD_UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#if defined(__GNUC__) || defined(__clang__)
#define BL_DIE_ATTR __attribute__((noreturn, format(printf, 1, 2)))
#define BL_FMT_ATTR(a, b) __attribute__((format(printf, a, b)))
#else
#define BL_DIE_ATTR
#define BL_FMT_ATTR(a, b)
#endif

/* Print "bubbleload: ..." to stderr and exit(1).  Flushes stdout first so
 * that --dry-run output is never interleaved with an error message. */
void die(const char *fmt, ...) BL_DIE_ATTR;
void warnmsg(const char *fmt, ...) BL_FMT_ATTR(1, 2);

void *xmalloc(size_t n);
void *xzalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) BL_FMT_ATTR(1, 2);

/* Shell-quote s onto out; safe to eval in a POSIX shell. */
void fput_quoted(FILE *out, const char *s);

/* $HOME, must be set and absolute.  The returned string is never freed. */
const char *home_dir(void);

/* realpath() + require that it is a directory.  Exits on failure.
 * what is the option name used in the error message (e.g. "-w"). */
char *resolve_dir(const char *what, const char *path);

char *get_cwd(void);

/* Join dir and name with a single '/', unless dir already ends in '/'. */
char *join_path(const char *dir, const char *name, const char *suffix);

const char *path_basename(const char *path);

/* 1 if name is executable and found in $PATH (or is a path itself), else 0. */
int find_in_path(const char *name);

/* mkdir -p with mode, honouring umask.  Exits on failure. */
void mkdir_p(const char *path, mode_t mode);

/* Refuse to read config files that we do not own or that are group/world
 * writable.  Exits on failure. */
void check_secure_file(const char *path);

/* [A-Za-z0-9._-]+, no leading dot (no path traversal, no hidden files). */
int valid_name(const char *name);

#endif /* BUBBLELOAD_UTIL_H */
