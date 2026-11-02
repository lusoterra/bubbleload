#include "config.h"

#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INCLUDE_DEPTH 8

enum dkind {
    D_INCLUDE,
    D_EXEC,
    D_RAW,
    D_DIE_WITH_PARENT,
    D_UNSHARE_ALL,
    D_SHARE_NET,
    D_NEW_SESSION,
    D_HOSTNAME,
    D_RO_BIND,
    D_RO_BIND_TRY,
    D_BIND,
    D_BIND_TRY,
    D_SYMLINK,
    D_DIR,
    D_TMPFS,
    D_PROC,
    D_DEV,
    D_CHDIR,
    D_SETENV,
    D_UNSETENV,
    D_MASK,
    D_MKDIR,
    D_RESOLV_CONF,
    D_PROJECT,
    D_CWD,
    D_EXTRA_BINDS
};

struct dspec {
    const char *name;
    enum dkind kind;
    int min_args;
    int max_args; /* -1: unlimited */
};

/* Table ordered by name for readability only; lookup is linear (tiny). */
static const struct dspec specs[] = {
    { "bind",            D_BIND,              1,  2 },
    { "bind-try",        D_BIND_TRY,          1,  2 },
    { "chdir",           D_CHDIR,             1,  1 },
    { "cwd",             D_CWD,               0,  0 },
    { "dev",             D_DEV,               1,  1 },
    { "die-with-parent", D_DIE_WITH_PARENT,   0,  0 },
    { "dir",             D_DIR,               1,  1 },
    { "exec",            D_EXEC,              1, -1 },
    { "extra-binds",     D_EXTRA_BINDS,       0,  0 },
    { "hostname",        D_HOSTNAME,          1,  1 },
    { "include",         D_INCLUDE,           1,  1 },
    { "mask",            D_MASK,              1,  1 },
    { "mkdir",           D_MKDIR,             1,  1 },
    { "new-session",     D_NEW_SESSION,       0,  0 },
    { "proc",            D_PROC,              1,  1 },
    { "project",         D_PROJECT,           0,  0 },
    { "raw",             D_RAW,               1, -1 },
    { "resolv-conf",     D_RESOLV_CONF,       0,  0 },
    { "ro-bind",         D_RO_BIND,           1,  2 },
    { "ro-bind-try",     D_RO_BIND_TRY,       1,  2 },
    { "setenv",          D_SETENV,            2,  2 },
    { "share-net",       D_SHARE_NET,         0,  0 },
    { "symlink",         D_SYMLINK,           2,  2 },
    { "tmpfs",           D_TMPFS,             1,  1 },
    { "unshare-all",     D_UNSHARE_ALL,       0,  0 },
    { "unsetenv",        D_UNSETENV,          1,  1 },
};

struct directive {
    enum dkind kind;
    struct strvec args; /* already expanded */
    char *file;
    int line;
};

struct loader {
    struct bl_emit *emit;
    const struct bl_ctx *ctx;
    const char *dir;
    struct directive *list;
    size_t len;
    size_t cap;
    struct strvec stack; /* canonical paths currently being parsed */
};

/* ---------------------------------------------------------------- buffers */

struct buf {
    char *p;
    size_t len;
    size_t cap;
};

static void buf_init(struct buf *b)
{
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

static void buf_putn(struct buf *b, const char *s, size_t n)
{
    if (b->len + n + 1u > b->cap) {
        size_t ncap = b->cap ? b->cap : 32u;

        while (ncap < b->len + n + 1u) {
            if (ncap > ((size_t)-1) / 2u) {
                die("token too long");
            }
            ncap *= 2u;
        }
        b->p = xrealloc(b->p, ncap);
        b->cap = ncap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_puts(struct buf *b, const char *s)
{
    buf_putn(b, s, strlen(s));
}

static char *buf_take(struct buf *b)
{
    char *p;

    if (b->p == NULL) {
        p = xstrdup("");
    } else {
        p = b->p;
    }
    buf_init(b);
    return p;
}

/* ------------------------------------------------------------- tokenizer */

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

static int ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int ident_char(char c)
{
    return ident_start(c) || (c >= '0' && c <= '9');
}

/* Split a line into tokens.  Double quotes group a token, a backslash inside
 * double quotes escapes the next character, '#' at the start of a token
 * starts a comment. */
static void tokenize(const char *line, const char *file, int lineno,
                     struct strvec *out)
{
    const char *p = line;

    for (;;) {
        struct buf b;
        int in_quotes = 0;

        while (is_space(*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            break;
        }
        buf_init(&b);
        while (*p != '\0') {
            if (in_quotes) {
                if (*p == '"') {
                    in_quotes = 0;
                    p++;
                } else if (*p == '\\') {
                    p++;
                    if (*p == '\0') {
                        die("%s:%d: unterminated double quote", file, lineno);
                    }
                    buf_putn(&b, p, 1u);
                    p++;
                } else {
                    buf_putn(&b, p, 1u);
                    p++;
                }
                continue;
            }
            if (*p == '"') {
                in_quotes = 1;
                p++;
                continue;
            }
            if (is_space(*p)) {
                break;
            }
            buf_putn(&b, p, 1u);
            p++;
        }
        if (in_quotes) {
            die("%s:%d: unterminated double quote", file, lineno);
        }
        strvec_push_owned(out, buf_take(&b));
        if (*p == '\0') {
            break;
        }
    }
}

/* -------------------------------------------------------------- expansion */

/* Returns 1 and sets *out when the variable is known and set, 2 when it is
 * known but unset, 0 when it is unknown. */
static int lookup_var(const struct bl_ctx *ctx, const char *name, size_t len,
                      const char **out)
{
    static char uidbuf[32];

    if (len == 4u && memcmp(name, "HOME", 4u) == 0) {
        *out = ctx->home;
        return 1;
    }
    if (len == 7u && memcmp(name, "PROJECT", 7u) == 0) {
        if (ctx->project == NULL) {
            return 2;
        }
        *out = ctx->project;
        return 1;
    }
    if (len == 3u && memcmp(name, "PWD", 3u) == 0) {
        *out = ctx->pwd;
        return 1;
    }
    if (len == 15u && memcmp(name, "XDG_RUNTIME_DIR", 15u) == 0) {
        const char *v = getenv("XDG_RUNTIME_DIR");

        if (v == NULL || v[0] == '\0') {
            return 2;
        }
        *out = v;
        return 1;
    }
    if (len == 3u && memcmp(name, "UID", 3u) == 0) {
        snprintf(uidbuf, sizeof uidbuf, "%ld", (long)getuid());
        *out = uidbuf;
        return 1;
    }
    return 0;
}

/* Expand $VAR and ${VAR}, then a leading "~". */
static char *expand_token(const struct bl_ctx *ctx, const char *tok,
                          const char *file, int lineno)
{
    struct buf b;
    const char *p = tok;

    buf_init(&b);
    if (tok[0] == '~') {
        if (tok[1] != '\0' && tok[1] != '/') {
            die("%s:%d: unsupported ~user expansion: %s", file, lineno, tok);
        }
        buf_puts(&b, ctx->home);
        p = tok + 1;
    }
    while (*p != '\0') {
        const char *name;
        size_t namelen;
        const char *value = NULL;
        int known;

        if (*p != '$') {
            buf_putn(&b, p, 1u);
            p++;
            continue;
        }
        if (p[1] == '{') {
            const char *close = strchr(p + 2, '}');

            if (close == NULL) {
                die("%s:%d: unterminated ${...}", file, lineno);
            }
            name = p + 2;
            namelen = (size_t)(close - name);
            p = close + 1;
        } else if (ident_start(p[1])) {
            name = p + 1;
            namelen = 0;
            while (ident_char(name[namelen])) {
                namelen++;
            }
            p = name + namelen;
        } else {
            buf_putn(&b, p, 1u);
            p++;
            continue;
        }
        if (namelen == 0u || !ident_start(name[0])) {
            die("%s:%d: invalid variable name in ${...}", file, lineno);
        }
        known = lookup_var(ctx, name, namelen, &value);
        if (known == 0) {
            die("%s:%d: unknown variable $%.*s", file, lineno, (int)namelen,
                name);
        }
        if (known == 2) {
            die("%s:%d: $%.*s is not set", file, lineno, (int)namelen, name);
        }
        buf_puts(&b, value);
    }
    return buf_take(&b);
}

/* ---------------------------------------------------------------- parsing */

static const struct dspec *lookup_spec(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        if (strcmp(specs[i].name, name) == 0) {
            return &specs[i];
        }
    }
    return NULL;
}

static void list_push(struct loader *ld, const struct directive *d)
{
    if (ld->len == ld->cap) {
        size_t ncap = ld->cap ? ld->cap * 2u : 16u;

        if (ncap < ld->cap) {
            die("too many directives");
        }
        ld->list = xrealloc(ld->list, ncap * sizeof *ld->list);
        ld->cap = ncap;
    }
    ld->list[ld->len++] = *d;
}

static void arity_error(const struct dspec *spec, const char *file, int lineno,
                        size_t got)
{
    if (spec->max_args < 0) {
        die("%s:%d: '%s' takes at least %d argument(s), got %zu", file, lineno,
            spec->name, spec->min_args, got);
    }
    if (spec->min_args == spec->max_args) {
        die("%s:%d: '%s' takes exactly %d argument(s), got %zu", file, lineno,
            spec->name, spec->min_args, got);
    }
    die("%s:%d: '%s' takes %d to %d arguments, got %zu", file, lineno,
        spec->name, spec->min_args, spec->max_args, got);
}

static char *include_chain(const struct strvec *stack, const char *path)
{
    struct buf b;
    size_t i;

    buf_init(&b);
    for (i = 0; i < stack->len; i++) {
        buf_puts(&b, stack->v[i]);
        buf_puts(&b, " -> ");
    }
    buf_puts(&b, path);
    return buf_take(&b);
}

static void parse_file(struct loader *ld, const char *path, int depth)
{
    FILE *f;
    char *line = NULL;
    size_t linecap = 0;
    ssize_t got;
    int lineno = 0;
    char *canon;
    size_t i;

    if (depth > MAX_INCLUDE_DEPTH) {
        die("%s: include depth limit (%d) exceeded", path, MAX_INCLUDE_DEPTH);
    }
    canon = realpath(path, NULL);
    if (canon == NULL) {
        die("%s: %s", path, strerror(errno));
    }
    for (i = 0; i < ld->stack.len; i++) {
        if (strcmp(ld->stack.v[i], canon) == 0) {
            char *chain = include_chain(&ld->stack, canon);

            die("include cycle detected: %s", chain);
        }
    }
    check_secure_file(path);
    f = fopen(path, "r");
    if (f == NULL) {
        die("%s: %s", path, strerror(errno));
    }
    strvec_push_owned(&ld->stack, canon);

    while ((got = getline(&line, &linecap, f)) != -1) {
        struct strvec toks;
        const struct dspec *spec;
        size_t nargs;

        lineno++;
        strvec_init(&toks);
        tokenize(line, path, lineno, &toks);
        if (toks.len == 0u) {
            strvec_free(&toks);
            continue;
        }
        spec = lookup_spec(toks.v[0]);
        if (spec == NULL) {
            die("%s:%d: unknown directive '%s'", path, lineno, toks.v[0]);
        }
        nargs = toks.len - 1u;
        if (nargs < (size_t)spec->min_args ||
            (spec->max_args >= 0 && nargs > (size_t)spec->max_args)) {
            arity_error(spec, path, lineno, nargs);
        }
        if (spec->kind == D_INCLUDE) {
            char *name = expand_token(ld->ctx, toks.v[1], path, lineno);
            char *ipath;

            if (!valid_name(name)) {
                die("%s:%d: invalid include name '%s'", path, lineno, name);
            }
            ipath = join_path(ld->dir, name, ".conf");
            parse_file(ld, ipath, depth + 1);
            free(ipath);
            free(name);
        } else {
            struct directive d;
            size_t a;

            d.kind = spec->kind;
            d.file = xstrdup(path);
            d.line = lineno;
            strvec_init(&d.args);
            for (a = 1u; a < toks.len; a++) {
                strvec_push_owned(&d.args,
                                  expand_token(ld->ctx, toks.v[a], path,
                                               lineno));
            }
            list_push(ld, &d);
        }
        strvec_free(&toks);
    }
    if (ferror(f)) {
        die("%s: read error: %s", path, strerror(errno));
    }
    fclose(f);
    free(line);
    free(ld->stack.v[--ld->stack.len]);
}

/* --------------------------------------------------------------- emission */

void bl_group(struct bl_emit *e)
{
    sizevec_push(&e->groups, e->argv.len);
}

void bl_arg(struct bl_emit *e, const char *arg)
{
    strvec_push(&e->argv, arg);
}

static void emit_bind(struct bl_emit *e, const char *flag,
                      const struct directive *d)
{
    bl_group(e);
    bl_arg(e, flag);
    bl_arg(e, d->args.v[0]);
    bl_arg(e, d->args.len > 1u ? d->args.v[1] : d->args.v[0]);
}

static void emit_extra_binds(struct bl_emit *e, const struct bl_ctx *ctx)
{
    size_t i;

    for (i = 0; i < ctx->ro->len; i++) {
        bl_group(e);
        bl_arg(e, "--ro-bind");
        bl_arg(e, ctx->ro->v[i]);
        bl_arg(e, ctx->ro->v[i]);
    }
    for (i = 0; i < ctx->rw->len; i++) {
        bl_group(e);
        bl_arg(e, "--bind");
        bl_arg(e, ctx->rw->v[i]);
        bl_arg(e, ctx->rw->v[i]);
    }
}

static void emit_project(struct bl_emit *e, const struct bl_ctx *ctx)
{
    bl_group(e);
    bl_arg(e, "--bind");
    bl_arg(e, ctx->project);
    bl_arg(e, ctx->project);
    bl_arg(e, "--chdir");
    bl_arg(e, ctx->project);
}

/* Original script logic: if /etc/resolv.conf is a symlink pointing outside
 * /etc (systemd-resolved), bind the real file to itself so the sandbox sees a
 * usable resolver too. */
static void emit_resolv_conf(struct bl_emit *e)
{
    char *real = realpath("/etc/resolv.conf", NULL);

    if (real == NULL) {
        return;
    }
    if (strncmp(real, "/etc/", 5u) != 0) {
        bl_group(e);
        bl_arg(e, "--ro-bind");
        bl_arg(e, real);
        bl_arg(e, real);
    }
    free(real);
}

static void emit_directive(struct bl_emit *e, const struct directive *d)
{
    size_t i;

    switch (d->kind) {
    case D_DIE_WITH_PARENT:
        bl_group(e);
        bl_arg(e, "--die-with-parent");
        return;
    case D_UNSHARE_ALL:
        bl_group(e);
        bl_arg(e, "--unshare-all");
        return;
    case D_SHARE_NET:
        bl_group(e);
        bl_arg(e, "--share-net");
        return;
    case D_NEW_SESSION:
        bl_group(e);
        bl_arg(e, "--new-session");
        return;
    case D_HOSTNAME:
        bl_group(e);
        bl_arg(e, "--hostname");
        bl_arg(e, d->args.v[0]);
        return;
    case D_RO_BIND:
        emit_bind(e, "--ro-bind", d);
        return;
    case D_RO_BIND_TRY:
        emit_bind(e, "--ro-bind-try", d);
        return;
    case D_BIND:
        emit_bind(e, "--bind", d);
        return;
    case D_BIND_TRY:
        emit_bind(e, "--bind-try", d);
        return;
    case D_SYMLINK:
        bl_group(e);
        bl_arg(e, "--symlink");
        bl_arg(e, d->args.v[0]);
        bl_arg(e, d->args.v[1]);
        return;
    case D_DIR:
        bl_group(e);
        bl_arg(e, "--dir");
        bl_arg(e, d->args.v[0]);
        return;
    case D_TMPFS:
        bl_group(e);
        bl_arg(e, "--tmpfs");
        bl_arg(e, d->args.v[0]);
        return;
    case D_PROC:
        bl_group(e);
        bl_arg(e, "--proc");
        bl_arg(e, d->args.v[0]);
        return;
    case D_DEV:
        bl_group(e);
        bl_arg(e, "--dev");
        bl_arg(e, d->args.v[0]);
        return;
    case D_CHDIR:
        bl_group(e);
        bl_arg(e, "--chdir");
        bl_arg(e, d->args.v[0]);
        return;
    case D_SETENV:
        bl_group(e);
        bl_arg(e, "--setenv");
        bl_arg(e, d->args.v[0]);
        bl_arg(e, d->args.v[1]);
        return;
    case D_UNSETENV:
        bl_group(e);
        bl_arg(e, "--unsetenv");
        bl_arg(e, d->args.v[0]);
        return;
    case D_MASK:
        bl_group(e);
        bl_arg(e, "--ro-bind");
        bl_arg(e, "/dev/null");
        bl_arg(e, d->args.v[0]);
        return;
    case D_RAW:
        bl_group(e);
        for (i = 0; i < d->args.len; i++) {
            bl_arg(e, d->args.v[i]);
        }
        return;
    default:
        die("internal error: unhandled directive at %s:%d", d->file, d->line);
    }
}

static void interpret(struct loader *ld)
{
    struct bl_emit *e = ld->emit;
    const struct bl_ctx *ctx = ld->ctx;
    int has_extra = 0;
    int has_project = 0;
    size_t i;

    for (i = 0; i < ld->len; i++) {
        if (ld->list[i].kind == D_EXTRA_BINDS) {
            has_extra = 1;
        } else if (ld->list[i].kind == D_PROJECT ||
                   ld->list[i].kind == D_CWD) {
            has_project = 1;
        }
    }

    for (i = 0; i < ld->len; i++) {
        const struct directive *d = &ld->list[i];

        switch (d->kind) {
        case D_INCLUDE:
            die("internal error: include directive was not inlined");
            break;
        case D_EXEC:
            strvec_free(&e->cmd);
            strvec_init(&e->cmd);
            for (size_t a = 0; a < d->args.len; a++) {
                strvec_push(&e->cmd, d->args.v[a]);
            }
            e->has_exec = 1;
            break;
        case D_MKDIR:
            /* Host side, applied right before exec. */
            strvec_push(&e->mkdirs, d->args.v[0]);
            break;
        case D_RESOLV_CONF:
            emit_resolv_conf(e);
            break;
        case D_EXTRA_BINDS:
            emit_extra_binds(e, ctx);
            break;
        case D_PROJECT:
            if (ctx->project == NULL) {
                warnmsg("%s:%d: $PROJECT is unset, skipping 'project' "
                        "(use -P DIR or --cwd to expose a directory)",
                        d->file, d->line);
                break;
            }
            /* Without an explicit "extra-binds" directive the command line
             * binds are placed just before the project mount. */
            if (!has_extra) {
                emit_extra_binds(e, ctx);
            }
            emit_project(e, ctx);
            break;
        case D_CWD:
            if (!ctx->allow_home && ctx->home_real != NULL &&
                strcmp(ctx->pwd, ctx->home_real) == 0) {
                warnmsg("%s:%d: current directory is $HOME, skipping 'cwd' "
                        "(use --allow-home)", d->file, d->line);
                break;
            }
            if (!has_extra) {
                emit_extra_binds(e, ctx);
            }
            bl_group(e);
            bl_arg(e, "--bind");
            bl_arg(e, ctx->pwd);
            bl_arg(e, ctx->pwd);
            bl_arg(e, "--chdir");
            bl_arg(e, ctx->pwd);
            break;
        default:
            emit_directive(e, d);
            break;
        }
    }
    if (!has_extra && !has_project) {
        emit_extra_binds(e, ctx);
    }
}

/* ------------------------------------------------------------------- API */

void bl_emit_init(struct bl_emit *e)
{
    strvec_init(&e->argv);
    sizevec_init(&e->groups);
    strvec_init(&e->mkdirs);
    strvec_init(&e->cmd);
    e->has_exec = 0;
    bl_group(e);
    bl_arg(e, "bwrap");
}

void bl_emit_free(struct bl_emit *e)
{
    strvec_free(&e->argv);
    sizevec_free(&e->groups);
    strvec_free(&e->mkdirs);
    strvec_free(&e->cmd);
    e->has_exec = 0;
}

void bl_load_profile(struct bl_emit *e, const struct bl_ctx *ctx,
                     const char *dir, const char *path)
{
    struct loader ld;
    size_t i;

    memset(&ld, 0, sizeof ld);
    ld.emit = e;
    ld.ctx = ctx;
    ld.dir = dir;
    strvec_init(&ld.stack);

    parse_file(&ld, path, 0);
    interpret(&ld);

    for (i = 0; i < ld.len; i++) {
        free(ld.list[i].file);
        strvec_free(&ld.list[i].args);
    }
    free(ld.list);
    strvec_free(&ld.stack);
}
