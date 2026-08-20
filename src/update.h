#ifndef EXO_UPDATE_H
#define EXO_UPDATE_H

#include <stddef.h>

int exo_update_available(char *info, size_t infosz);
int exo_update_self(const char *argv0);
void exo_update_banner(const char *argv0);

#endif