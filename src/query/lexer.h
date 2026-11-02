#ifndef FSX_LEXER_H
#define FSX_LEXER_H
#include <stddef.h>
enum {
  FS_T_END,
  FS_T_WORD,
  FS_T_NOT,
  FS_T_OR,
  FS_T_OPEN,
  FS_T_CLOSE,
  FS_T_INVALID
};
typedef struct FsToken {
  int kind;
  const char *begin;
  size_t length;
} FsToken;
int fsx_lex(const char **cursor, FsToken *out);
#endif
