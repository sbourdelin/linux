#ifndef __PERF_ZLIB_H
#define __PERF_ZLIB_H

#ifdef HAVE_ZLIB_SUPPORT
int gzip_decompress_to_file(const char *input, int output_fd);
#endif

#endif /* __PERF_ZLIB_H */
