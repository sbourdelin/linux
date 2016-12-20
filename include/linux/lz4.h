#ifndef __LZ4_H__
#define __LZ4_H__

/*
 * LZ4 Kernel Interface
 *
 * Copyright (C) 2016, Sven Schmidt <4sschmid@informatik.uni-hamburg.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/types.h>

/*-************************************************************************
*  Version
**************************************************************************/
#define LZ4_VERSION_MAJOR    1    /* for breaking interface changes  */
#define LZ4_VERSION_MINOR    7    /* for new (non-breaking) interface capabilities */
#define LZ4_VERSION_RELEASE  2    /* for tweaks, bug-fixes, or development */

#define LZ4_VERSION_NUMBER (LZ4_VERSION_MAJOR *100*100 + LZ4_VERSION_MINOR *100 + LZ4_VERSION_RELEASE)

/*-************************************************************************
*  Constants
**************************************************************************/
#define LZ4_MEM_COMPRESS	(16384)
#define LZ4HC_MEM_COMPRESS	(262144 + (2 * sizeof(unsigned char *)))

#define LZ4_MAX_INPUT_SIZE        0x7E000000   /* 2 113 929 216 bytes */
#define LZ4_COMPRESSBOUND(isize)  ((unsigned)(isize) > (unsigned)LZ4_MAX_INPUT_SIZE ? 0 : (isize) + ((isize)/255) + 16)

#define LZ4HC_MIN_CLEVEL        3
#define LZ4HC_DEFAULT_CLEVEL    9
#define LZ4HC_MAX_CLEVEL        16

/*-************************************************************************
*  Compression Functions
**************************************************************************/

/*!LZ4_compressbound() :
    Provides the maximum size that LZ4 may output in a "worst case" scenario
    (input data not compressible)
*/
static inline size_t LZ4_compressBound(size_t isize) {
  return LZ4_COMPRESSBOUND(isize);
}

/*! lz4_compressbound() :
    For backward compabitiliby
*/
static inline size_t lz4_compressbound(size_t isize) {
	return lz4_compressbound(isize);
}

/*! LZ4_compress_default() :
    Compresses 'sourceSize' bytes from buffer 'source'
    into already allocated 'dest' buffer of size 'maxDestSize'.
    Compression is guaranteed to succeed if 'maxDestSize' >= LZ4_compressBound(sourceSize).
    It also runs faster, so it's a recommended setting.
    If the function cannot compress 'source' into a more limited 'dest' budget,
    compression stops *immediately*, and the function result is zero.
    As a consequence, 'dest' content is not valid.
    This function never writes outside 'dest' buffer, nor read outside 'source' buffer.
        sourceSize  : Max supported value is LZ4_MAX_INPUT_VALUE
        maxDestSize : full or partial size of buffer 'dest' (which must be already allocated)
        workmem : address of the working memory. This requires 'workmem' of size LZ4_MEM_COMPRESS.
        return : the number of bytes written into buffer 'dest' (necessarily <= maxOutputSize)
              or 0 if compression fails */
int LZ4_compress_default(const char* source, char* dest, int inputSize, int maxOutputSize, void* wrkmem);

/*!
LZ4_compress_fast() :
    Same as LZ4_compress_default(), but allows to select an "acceleration" factor.
    The larger the acceleration value, the faster the algorithm, but also the lesser the compression.
    It's a trade-off. It can be fine tuned, with each successive value providing roughly +~3% to speed.
    An acceleration value of "1" is the same as regular LZ4_compress_default()
    Values <= 0 will be replaced by ACCELERATION_DEFAULT, which is 1.
*/
int LZ4_compress_fast(const char* source, char* dest, int inputSize, int maxOutputSize, void* wrkmem, int acceleration);

/*!
LZ4_compress_destSize() :
    Reverse the logic, by compressing as much data as possible from 'source' buffer
    into already allocated buffer 'dest' of size 'targetDestSize'.
    This function either compresses the entire 'source' content into 'dest' if it's large enough,
    or fill 'dest' buffer completely with as much data as possible from 'source'.
        *sourceSizePtr : will be modified to indicate how many bytes where read from 'source' to fill 'dest'.
                         New value is necessarily <= old value.
        workmem : address of the working memory. This requires 'workmem' of size LZ4_MEM_COMPRESS.
        return : Nb bytes written into 'dest' (necessarily <= targetDestSize)
              or 0 if compression fails
*/
int LZ4_compress_destSize (const char* source, char* dest, int* sourceSizePtr, int targetDestSize, void* wrkmem);

/*!
 * lz4_compress() :
    For backwards compabitiliby
        src     : source address of the original data
        src_len : size of the original data
        dst     : output buffer address of the compressed data
                This requires 'dst' of size LZ4_COMPRESSBOUND.
        dst_len : is the output size, which is returned after compress done
        workmem : address of the working memory.
                This requires 'workmem' of size LZ4_MEM_COMPRESS.
        return  : Success if return 0
                  Error if return (< 0)
        note :  Destination buffer and workmem must be already allocated with
                the defined size.
 */
int lz4_compress(const unsigned char *src, int src_len, unsigned char *dst, int dst_len, void *wrkmem);

/*-************************************************************************
*  Decompression Functions
**************************************************************************/

/*!
LZ4_decompress_fast() :
    originalSize : is the original and therefore uncompressed size
    return : the number of bytes read from the source buffer (in other words, the compressed size)
             If the source stream is detected malformed, the function will stop decoding and return a negative result.
             Destination buffer must be already allocated. Its size must be a minimum of 'originalSize' bytes.
    note : This function fully respect memory boundaries for properly formed compressed data.
           It is a bit faster than LZ4_decompress_safe().
           However, it does not provide any protection against intentionally modified data stream (malicious input).
           Use this function in trusted environment only (data to decode comes from a trusted source).
*/
int LZ4_decompress_fast(const char* source, char* dest, int originalSize);

/*!
LZ4_decompress_safe() :
    compressedSize : is the precise full size of the compressed block.
    maxDecompressedSize : is the size of destination buffer, which must be already allocated.
    return : the number of bytes decompressed into destination buffer (necessarily <= maxDecompressedSize)
             If destination buffer is not large enough, decoding will stop and output an error code (<0).
             If the source stream is detected malformed, the function will stop decoding and return a negative result.
             This function is protected against buffer overflow exploits, including malicious data packets.
             It never writes outside output buffer, nor reads outside input buffer.
*/
int LZ4_decompress_safe(const char* source, char* dest, int compressedSize, int maxDecompressedSize);

/*!
LZ4_decompress_safe_partial() :
    This function decompress a compressed block of size 'compressedSize' at position 'source'
    into destination buffer 'dest' of size 'maxDecompressedSize'.
    The function tries to stop decompressing operation as soon as 'targetOutputSize' has been reached,
    reducing decompression time.
    return : the number of bytes decoded in the destination buffer (necessarily <= maxDecompressedSize)
       Note : this number can be < 'targetOutputSize' should the compressed block to decode be smaller.
             Always control how many bytes were decoded.
             If the source stream is detected malformed, the function will stop decoding and return a negative result.
             This function never writes outside of output buffer, and never reads outside of input buffer. It is therefore protected against malicious data packets
*/
int LZ4_decompress_safe_partial(const char* source, char* dest, int compressedSize, int targetOutputSize, int maxDecompressedSize);


/*!
lz4_decompress_unknownoutputsize() :
    For backwards compabitiliby
        src     : source address of the compressed data
        src_len : is the input size, therefore the compressed size
        dest    : output buffer address of the decompressed data
        dest_len: is the max size of the destination buffer, which is
                        returned with actual size of decompressed data after
                        decompress done
        return  : Success if return 0
                  Error if return (< 0)
        note :  Destination buffer must be already allocated.
*/
int lz4_decompress_unknownoutputsize(const unsigned char *src, size_t src_len, unsigned char *dest, size_t *dest_len);

/*!
lz4_decompress() :
    For backwards compabitiliby
        src     : source address of the compressed data
        src_len : is the input size, whcih is returned after decompress done
        dest    : output buffer address of the decompressed data
        actual_dest_len: is the size of uncompressed data, supposing it's known
        return  : Success if return 0
                  Error if return (< 0)
        note :  Destination buffer must be already allocated.
                slightly faster than lz4_decompress_unknownoutputsize()
*/
int lz4_decompress(const unsigned char *src, size_t *src_len, unsigned char *dest, size_t actual_dest_len);

/*-************************************************************************
 *  LZ4 HC Compression
 **************************************************************************/

/*! LZ4_compress_HC() :
    Compress data from `src` into `dst`, using the more powerful but slower "HC" algorithm. dst` must be already allocated.
        wrkmem : address of the working memory. This requires 'wrkmem' of size LZ4HC_MEM_COMPRESS.
        Compression is guaranteed to succeed if `dstCapacity >= LZ4_compressBound(srcSize)`
        Max supported `srcSize` value is LZ4_MAX_INPUT_SIZE
        compressionLevel` : Recommended values are between 4 and 9, although any value between 1 and LZ4HC_MAX_CLEVEL will work.
                        Values >LZ4HC_MAX_CLEVEL behave the same as 16.
        @return : the number of bytes written into 'dst' or 0 if compression fails.
*/
int LZ4_compress_HC(const char* src, char* dst, int srcSize, int dstCapacity, int compressionLevel, void* wrkmem);

/*! lz4hc_compress()
    For backwards compabitiliby
        src     : source address of the original data
        src_len : size of the original data
        dst     : output buffer address of the compressed data
               This requires 'dst' of size LZ4_COMPRESSBOUND.
        dst_len : is the output size, which is returned after compress done
        workmem : address of the working memory.
               This requires 'workmem' of size LZ4HC_MEM_COMPRESS.
        return  : Success if return 0
                  Error if return (< 0)
        note :  Destination buffer and workmem must be already allocated with
                the defined size.
*/
int lz4hc_compress(const unsigned char *src, size_t src_len, unsigned char *dst, size_t *dst_len, void *wrkmem);

#endif
