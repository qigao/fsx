#include "lexer.h"
int fsx_lex(const char **cursor,FsToken *out) {
    const char *YYCURSOR=*cursor,*YYMARKER,*start;
again:
    start=YYCURSOR;
    /*!re2c
        re2c:define:YYCTYPE = char;
        re2c:yyfill:enable = 0;
        quoted = '"' ([^"\\\x00] | "\\" [^\x00])* '"';
        plain = [^\x00 \t\r\n!|<>"]+;
        word = (quoted | plain)+;
        [ \t\r\n]+ { goto again; }
        "\x00" { out->kind=FS_T_END; --YYCURSOR; goto done; }
        "!" { out->kind=FS_T_NOT; goto done; }
        "|" { out->kind=FS_T_OR; goto done; }
        "<" { out->kind=FS_T_OPEN; goto done; }
        ">" { out->kind=FS_T_CLOSE; goto done; }
        "size:" ([<>=] "="?)? word { out->kind=FS_T_WORD; goto done; }
        word { out->kind=FS_T_WORD; goto done; }
        * { out->kind=FS_T_INVALID; goto done; }
    */
done:
    *cursor=YYCURSOR; out->begin=start; out->length=(size_t)(YYCURSOR-start);
    return out->kind;
}
