#ifndef LIBCHDR_MACROS_H
#define LIBCHDR_MACROS_H

#undef ARRAY_LENGTH
#define ARRAY_LENGTH(x) (sizeof(x)/sizeof(x[0]))

#undef MAX
#undef MIN
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#endif /* LIBCHDR_MACROS_H */
