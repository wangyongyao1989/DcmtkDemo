#ifndef DCMTK_CONFIG_ARITH_H
#define DCMTK_CONFIG_ARITH_H

/* 基础类型大小 (arm64-v8a LP64 模型) */
#define SIZEOF_CHAR 1
#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOID_P 8
#define SIZEOF_SIZE_T 8
#define SIZEOF_PTRDIFF_T 8
#define SIZEOF_WCHAR_T 4

/* 字节序：arm64 为小端 */
#undef WORDS_BIGENDIAN
#define DCMTK_WORDS_BIGENDIAN 0

/* char 默认无符号（Android arm64） */
#define DCMTK_CHAR_IS_UNSIGNED 1

#endif
