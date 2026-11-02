#ifndef BUBBLELOAD_CONFIG_H
#define BUBBLELOAD_CONFIG_H

#include "argv.h"

/* Values available to $VAR / ~ expansion in profile files. */
struct bl_ctx {
    const char *home;        /* $HOME as set in the environment */
    const char *home_real;   /* realpath() of $HOME, for the refusal check */
    const char *project;     /* absolute project directory, or NULL */
    const char *pwd;         /* absolute current working directory */
    const struct strvec *ro; /* absolute --ro directories, in order */
    const struct strvec *rw; /* absolute --rw directories, in order */
    int allow_home;          /* --allow-home / --dangerous */
};

/* Result of translating a profile into a bwrap invocation. */
struct bl_emit {
    struct strvec argv;        /* bwrap argv, argv.v[0] == "bwrap" */
    struct sizevec groups;     /* indexes into argv: one per printed line */
    struct strvec mkdirs;      /* host side mkdir -p paths, in file order */
    struct strvec cmd;         /* arguments of the "exec" directive */
    int has_exec;
};

void bl_emit_init(struct bl_emit *e);
void bl_emit_free(struct bl_emit *e);

/* Mark the start of a "flag group" (one line in --dry-run output). */
void bl_group(struct bl_emit *e);
void bl_arg(struct bl_emit *e, const char *arg);

/* Parse the profile at path (found in directory dir, used for includes) and
 * translate it into e.  Includes are resolved relative to dir.  Exits with a
 * diagnostic on any error. */
void bl_load_profile(struct bl_emit *e, const struct bl_ctx *ctx,
                     const char *dir, const char *path);

#endif /* BUBBLELOAD_CONFIG_H */
