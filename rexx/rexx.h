#ifndef REXX_H
#define REXX_H

#include <stdint.h>
#include <stddef.h>
/* Forward declarations */
typedef struct AST AST;
typedef struct Context Context;
typedef int (*BuiltinFunc)(Context* ctx, AST* args);

void copy_name(char* dst, const char* src, size_t len);
int names_equal(const char* a, const char* b);

typedef int (*BuiltinFunc)(Context* ctx, AST* args);

#define MAX_BUILTINS 64

typedef struct {
    char name[32];
    BuiltinFunc fn;
} Builtin;

/* Tokens */

typedef enum {
    TOK_EOF,
    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_STRING,

    TOK_SAY,
    TOK_EXIT,

    TOK_IF,
    TOK_THEN,
    TOK_ELSE,

    TOK_ASSIGN,     // =
    TOK_PLUS,       // +
    TOK_MINUS,      // -
    TOK_MUL,        // *
    TOK_DIV,        // /

    TOK_LPAREN,
    TOK_RPAREN,
    TOK_GT,
    TOK_LT,
    TOK_EQ,
    TOK_DO,
    TOK_BEGIN,
    TOK_END,
    TOK_PROC,
    TOK_CALL

} TokenType;

typedef struct {
    TokenType type;
    const char* start;
    size_t length;
} Token;

/* Lexer */

typedef struct {
    const char* src;
    size_t pos;
} Lexer;

void lexer_init(Lexer* l, const char* src);
Token lexer_next(Lexer* l);

/* Runtime */

#define MAX_VARS 128

typedef struct {
    char name[32];
    char value[128];
} Variable;

/* Procedures */

#define MAX_PROCS 64

typedef struct {
    char name[32];
    struct AST* body;
} Procedure;

/* Execution context */

struct Context {
    Variable vars[MAX_VARS];
    size_t var_count;

    Procedure procs[MAX_PROCS];
    size_t proc_count;

    Builtin builtins[MAX_BUILTINS];
    size_t builtin_count;
};


/* AST */

typedef enum {
    AST_NUMBER,
    AST_STRING,
    AST_VAR,
    AST_ASSIGN,
    AST_ADD,
    AST_SAY,
    AST_EXIT,
    AST_IF,
    AST_DO,
    AST_BLOCK,
    AST_PROC,
    AST_CALL


} ASTType;

typedef struct AST {
    ASTType type;

    struct AST* left;
    struct AST* right;
    struct AST* third;
    struct AST* next;



    /* For literals and variables */
    const char* value;
    size_t length;

    /* For binary operations */
    TokenType op;
} AST;

/* Parser */

typedef struct {
    Lexer lexer;
    Token current;
} Parser;

void parser_init(Parser* p, const char* src);
AST* parse_statement(Parser* p);
AST* orexx_parse_statement(Parser* p);



void runtime_init(Context* ctx);
void runtime_set(Context* ctx, const char* name, const char* value);
const char* runtime_get(Context* ctx, const char* name);

/* Evaluation */

void eval(Context* ctx, AST* node);

#endif
