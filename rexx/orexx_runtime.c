/* ============================================================================
 * rexx/orexx_parser.c  —  Object REXX Parser Extension
 *
 * Extends the REXX parser to recognize Object REXX syntax:
 *   - ::CLASS name [SUBCLASS parent]
 *   - ::METHOD methodname
 *   - EXPOSE variable-list
 *   - .ClassName~new()
 *   - object~method(args)
 * ========================================================================== */

#include "rexx.h"
#include "orexx.h"

/* ── Token Types for Object REXX ─────────────────────────────────────────── */

typedef enum {
    OREXX_TOK_DIRECTIVE,      /* :: */
    OREXX_TOK_CLASS,          /* CLASS */
    OREXX_TOK_METHOD,         /* METHOD */
    OREXX_TOK_SUBCLASS,       /* SUBCLASS */
    OREXX_TOK_PUBLIC,         /* PUBLIC */
    OREXX_TOK_PRIVATE,        /* PRIVATE */
    OREXX_TOK_PROTECTED,      /* PROTECTED */
    OREXX_TOK_EXPOSE,         /* EXPOSE */
    OREXX_TOK_USE,            /* USE */
    OREXX_TOK_ARG,            /* ARG */
    OREXX_TOK_TILDE,          /* ~ */
    OREXX_TOK_DOT,            /* . */
    OREXX_TOK_IDENTIFIER,
    OREXX_TOK_EOF
} ORexxTokenType;

/* ── AST Node Types for Object REXX ──────────────────────────────────────── */

typedef enum {
    OREXX_AST_CLASS_DEF,      /* Class definition */
    OREXX_AST_METHOD_DEF,     /* Method definition */
    OREXX_AST_MESSAGE_SEND,   /* object~method() */
    OREXX_AST_EXPOSE,         /* EXPOSE variable-list */
    OREXX_AST_CLASS_REF,      /* .ClassName */
    OREXX_AST_NEW_INSTANCE    /* .Class~new() */
} ORexxASTType;

/* ── Extended AST Nodes ──────────────────────────────────────────────────── */

typedef struct ORexxClassDef {
    char         *name;           /* Class name */
    char         *superclass;     /* Parent class (NULL = Object) */
    AST         **methods;        /* Array of method definitions */
    uint32_t      method_count;   /* Number of methods */
    uint32_t      flags;          /* PUBLIC, PRIVATE, etc. */
} ORexxClassDef;

typedef struct ORexxMethodDef {
    char         *name;           /* Method name */
    char        **expose_vars;    /* EXPOSE variable list */
    uint32_t      expose_count;   /* Number of exposed variables */
    AST          *body;           /* Method body (statements) */
    uint32_t      flags;          /* PUBLIC, PRIVATE, PROTECTED */
} ORexxMethodDef;

typedef struct ORexxMessageSend {
    AST          *receiver;       /* Object receiving message */
    char         *selector;       /* Method name */
    AST         **args;           /* Argument expressions */
    uint32_t      arg_count;      /* Number of arguments */
} ORexxMessageSend;

/* ── Parser State Extensions ─────────────────────────────────────────────── */

typedef struct ORexxParserState {
    Parser       *base_parser;    /* Original REXX parser */
    const char   *source;         /* Current position in source */
    int           in_class;       /* Parsing inside ::CLASS block */
    int           in_method;      /* Parsing inside ::METHOD block */
    char         *current_class;  /* Name of class being defined */
} ORexxParserState;

/* ── Keyword matching ───────────────────────────────────────────────────── */

static int orexx_match_keyword(const char **src, const char *keyword)
{
    const char *s = *src;
    const char *k = keyword;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t') s++;

    /* Match keyword (case insensitive) */
    while (*k) {
        char c1 = *s;
        char c2 = *k;

        /* Convert to uppercase for comparison */
        if (c1 >= 'a' && c1 <= 'z') c1 = c1 - 'a' + 'A';
        if (c2 >= 'a' && c2 <= 'z') c2 = c2 - 'a' + 'A';

        if (c1 != c2) return 0;
        s++;
        k++;
    }

    /* Ensure keyword ends with delimiter */
    if (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
        return 0;
    }

    *src = s;
    return 1;
}

/* ── Helper: Parse identifier ────────────────────────────────────────────── */

static char* orexx_parse_identifier(const char **src)
{
    const char *s = *src;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t') s++;

    /* Identifier must start with letter or underscore */
    if ((*s < 'A' || *s > 'Z') && (*s < 'a' || *s > 'z') && *s != '_') {
        return NULL;
    }

    const char *start = s;

    /* Continue with letters, digits, or underscore */
    while ((*s >= 'A' && *s <= 'Z') ||
           (*s >= 'a' && *s <= 'z') ||
           (*s >= '0' && *s <= '9') ||
           *s == '_') {
        s++;
    }

    /* Allocate and copy identifier */
    size_t len = s - start;
    char *id = mem_alloc(len + 1);  /* TODO: Use proper allocator */
    if (!id) return NULL;

    for (size_t i = 0; i < len; i++) {
        id[i] = start[i];
    }
    id[len] = '\0';

    *src = s;
    return id;
}

/* Parse ::CLASS name [SUBCLASS parent] [PUBLIC|PRIVATE]. */

AST* orexx_parse_class_directive(ORexxParserState *state)
{
    const char *s = state->source;

    /* Match ::CLASS */
    if (*s != ':' || *(s+1) != ':') return NULL;
    s += 2;

    if (!orexx_match_keyword(&s, "CLASS")) return NULL;

    /* Parse class name */
    char *classname = orexx_parse_identifier(&s);
    if (!classname) return NULL;

    char *superclass = NULL;
    if (orexx_match_keyword(&s, "SUBCLASS")) {
        superclass = orexx_parse_identifier(&s);
    }

    uint32_t flags = OREXX_METHOD_PUBLIC;  /* Default PUBLIC */
    if (orexx_match_keyword(&s, "PUBLIC")) {
        flags = OREXX_METHOD_PUBLIC;
    } else if (orexx_match_keyword(&s, "PRIVATE")) {
        flags = OREXX_METHOD_PRIVATE;
    }

    AST *node = mem_alloc(sizeof(AST));  /* TODO: Use proper allocator */
    if (!node) return NULL;

    node->type = OREXX_AST_CLASS_DEF;

    ORexxClassDef *classdef = mem_alloc(sizeof(ORexxClassDef));
    classdef->name = classname;
    classdef->superclass = superclass;
    classdef->methods = NULL;
    classdef->method_count = 0;
    classdef->flags = flags;

    node->value = (char*)classdef;  /* Store class definition */

    state->source = s;
    state->in_class = 1;
    state->current_class = classname;

    return node;
}

/* Parse ::METHOD name [PUBLIC|PRIVATE|PROTECTED]. */

AST* orexx_parse_method_directive(ORexxParserState *state)
{
    const char *s = state->source;

    /* Match ::METHOD */
    if (*s != ':' || *(s+1) != ':') return NULL;
    s += 2;

    if (!orexx_match_keyword(&s, "METHOD")) return NULL;

    /* Parse method name */
    char *methodname = orexx_parse_identifier(&s);
    if (!methodname) return NULL;

    uint32_t flags = OREXX_METHOD_PUBLIC;  /* Default PUBLIC */
    if (orexx_match_keyword(&s, "PUBLIC")) {
        flags = OREXX_METHOD_PUBLIC;
    } else if (orexx_match_keyword(&s, "PRIVATE")) {
        flags = OREXX_METHOD_PRIVATE;
    } else if (orexx_match_keyword(&s, "PROTECTED")) {
        flags = OREXX_METHOD_PROTECTED;
    }

    AST *node = mem_alloc(sizeof(AST));
    if (!node) return NULL;

    node->type = OREXX_AST_METHOD_DEF;

    ORexxMethodDef *methoddef = mem_alloc(sizeof(ORexxMethodDef));
    methoddef->name = methodname;
    methoddef->expose_vars = NULL;
    methoddef->expose_count = 0;
    methoddef->body = NULL;
    methoddef->flags = flags;

    node->value = (char*)methoddef;

    state->source = s;
    state->in_method = 1;

    return node;
}

/* Parse an EXPOSE statement listing variable names. */

AST* orexx_parse_expose(ORexxParserState *state)
{
    const char *s = state->source;

    /* Match EXPOSE keyword */
    if (!orexx_match_keyword(&s, "EXPOSE")) return NULL;

    /* Parse variable list */
    char **vars = mem_alloc(sizeof(char*) * 32);  /* Max 32 variables */
    uint32_t count = 0;

    while (count < 32) {
        char *var = orexx_parse_identifier(&s);
        if (!var) break;

        vars[count++] = var;

        /* Skip whitespace */
        while (*s == ' ' || *s == '\t') s++;

        if (*s == '\n' || *s == '\r' || *s == '\0') break;
    }

    if (count == 0) return NULL;

    AST *node = mem_alloc(sizeof(AST));
    node->type = OREXX_AST_EXPOSE;
    node->value = (char*)vars;
    node->length = count;  /* Store count in length field */

    state->source = s;
    return node;
}

/* Parse object~method(args) and .ClassName~new(args) message sends. */

AST* orexx_parse_message_send(ORexxParserState *state, AST *receiver)
{
    const char *s = state->source;

    /* Match tilde operator */
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '~') return NULL;
    s++;

    /* Parse method name */
    char *method = orexx_parse_identifier(&s);
    if (!method) return NULL;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t') s++;

    /* Parse arguments if present */
    AST **args = NULL;
    uint32_t arg_count = 0;

    if (*s == '(') {
        s++;
        args = mem_alloc(sizeof(AST*) * 16);  /* Max 16 args */

        while (arg_count < 16) {
            /* Skip whitespace */
            while (*s == ' ' || *s == '\t') s++;

            if (*s == ')') {
                s++;
                break;
            }

            /* Parse argument expression */
            /* Preserve the argument text until the main expression parser is available. */
            const char *arg_start = s;
            while (*s && *s != ',' && *s != ')') s++;

            /* Preserve the source text as a string literal node. */
            AST *arg = mem_alloc(sizeof(AST));
            arg->type = 0;  /* String literal */
            size_t arg_len = s - arg_start;
            arg->value = mem_alloc(arg_len + 1);
            for (size_t i = 0; i < arg_len; i++) {
                arg->value[i] = arg_start[i];
            }
            arg->value[arg_len] = '\0';

            args[arg_count++] = arg;

            if (*s == ',') s++;
        }
    }

    AST *node = mem_alloc(sizeof(AST));
    node->type = OREXX_AST_MESSAGE_SEND;

    ORexxMessageSend *msg = mem_alloc(sizeof(ORexxMessageSend));
    msg->receiver = receiver;
    msg->selector = method;
    msg->args = args;
    msg->arg_count = arg_count;

    node->value = (char*)msg;

    state->source = s;
    return node;
}

/* Parse a .ClassName reference. */

AST* orexx_parse_class_reference(ORexxParserState *state)
{
    const char *s = state->source;

    /* Match dot */
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '.') return NULL;
    s++;

    /* Parse class name */
    char *classname = orexx_parse_identifier(&s);
    if (!classname) return NULL;

    AST *node = mem_alloc(sizeof(AST));
    node->type = OREXX_AST_CLASS_REF;
    node->value = classname;

    state->source = s;

    /* Continue parsing a chained message send when '~' follows. */
    const char *peek = s;
    while (*peek == ' ' || *peek == '\t') peek++;
    if (*peek == '~') {
        state->source = peek;
        return orexx_parse_message_send(state, node);
    }

    return node;
}

/* Object REXX parser entry point */

AST* orexx_parse_statement(Parser *parser)
{
    ORexxParserState state;
    state.base_parser = parser;
    state.source = parser->pos;  /* Assume parser has pos field */
    state.in_class = 0;
    state.in_method = 0;
    state.current_class = NULL;

    const char *s = state.source;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    state.source = s;

    if (*s == ':' && *(s+1) == ':') {
        /* Try ::CLASS */
        AST *node = orexx_parse_class_directive(&state);
        if (node) {
            parser->pos = state.source;
            return node;
        }

        /* Try ::METHOD */
        node = orexx_parse_method_directive(&state);
        if (node) {
            parser->pos = state.source;
            return node;
        }
    }

    if (state.in_method && orexx_match_keyword(&s, "EXPOSE")) {
        state.source = s - 6;  /* Back up to start of EXPOSE */
        AST *node = orexx_parse_expose(&state);
        if (node) {
            parser->pos = state.source;
            return node;
        }
    }

    if (*s == '.') {
        AST *node = orexx_parse_class_reference(&state);
        if (node) {
            parser->pos = state.source;
            return node;
        }
    }

    /* The main expression parser must recognize the Object REXX ~ operator. */

    /* Not an Object REXX construct - let main parser handle it */
    return NULL;
}

/* The main statement parser must call this before its standard REXX parser. */
