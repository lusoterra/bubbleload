#ifndef BUBBLELOAD_ARGV_H
#define BUBBLELOAD_ARGV_H

#include <stddef.h>

/* Small growable string vector.  Every string is owned by the vector. */
struct strvec {
    char **v;
    size_t len;
    size_t cap;
};

/* Growable vector of size_t, used for argv group boundaries. */
struct sizevec {
    size_t *v;
    size_t len;
    size_t cap;
};

void strvec_init(struct strvec *sv);
void strvec_push(struct strvec *sv, const char *s);   /* copies s */
void strvec_push_owned(struct strvec *sv, char *s);   /* takes ownership */
/* NUL-terminate the vector (without counting the NULL in len) so that v can
 * be handed to execvp() directly. */
void strvec_terminate(struct strvec *sv);
void strvec_free(struct strvec *sv);

void sizevec_init(struct sizevec *sv);
void sizevec_push(struct sizevec *sv, size_t value);
void sizevec_free(struct sizevec *sv);

#endif /* BUBBLELOAD_ARGV_H */
