#ifndef __PERF_LZMA_H
#define __PERF_LZMA_H

#ifdef HAVE_LZMA_SUPPORT
int lzma_decompress_to_file(const char *input, int output_fd);
#endif

#endif /* __PERF_LZMA_H */
