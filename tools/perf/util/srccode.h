#ifndef SRCLINE_H
#define SRCLINE_H 1

/* Result is not 0 terminated */
char *find_sourceline(char *fn, unsigned line, int *lenp);

#endif
