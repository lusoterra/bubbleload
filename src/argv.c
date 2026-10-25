#include "argv.h"

#include "util.h"

#include <stdlib.h>

#define VEC_MIN_CAP 8u

static void *grow(void *base, size_t *cap, size_t want, size_t elem)
{
    size_t ncap = *cap ? *cap : VEC_MIN_CAP;

    if (want <= *cap) {
        return base;
    }
    while (ncap < want) {
        if (ncap > ((size_t)-1) / 2u / elem) {
            die("vector too large");
        }
        ncap *= 2u;
    }
    *cap = ncap;
    return xrealloc(base, ncap * elem);
}

void strvec_init(struct strvec *sv)
{
    sv->v = NULL;
    sv->len = 0;
    sv->cap = 0;
}

void strvec_push(struct strvec *sv, const char *s)
{
    strvec_push_owned(sv, xstrdup(s));
}

void strvec_push_owned(struct strvec *sv, char *s)
{
    sv->v = grow(sv->v, &sv->cap, sv->len + 1u, sizeof *sv->v);
    sv->v[sv->len++] = s;
}

void strvec_terminate(struct strvec *sv)
{
    sv->v = grow(sv->v, &sv->cap, sv->len + 1u, sizeof *sv->v);
    sv->v[sv->len] = NULL;
}

void strvec_free(struct strvec *sv)
{
    size_t i;

    for (i = 0; i < sv->len; i++) {
        free(sv->v[i]);
    }
    free(sv->v);
    strvec_init(sv);
}

void sizevec_init(struct sizevec *sv)
{
    sv->v = NULL;
    sv->len = 0;
    sv->cap = 0;
}

void sizevec_push(struct sizevec *sv, size_t value)
{
    sv->v = grow(sv->v, &sv->cap, sv->len + 1u, sizeof *sv->v);
    sv->v[sv->len++] = value;
}

void sizevec_free(struct sizevec *sv)
{
    free(sv->v);
    sizevec_init(sv);
}
