/* ============================================================================
 * kernel/src/basic.c  —  QuickBASIC-compatible interpreter
 *
 * Dialect: QB 4.5 subset (line-numbered + structured, direct mode)
 *
 * Statements:
 *   PRINT   expr | "str" [; | , ...]        TAB(), SPC()
 *   LET     var = expr                       (LET optional)
 *   INPUT   ["prompt";] var
 *   IF      expr THEN stmt [ELSE stmt]       single-line
 *   IF      expr THEN / ELSEIF / ELSE / END IF   block form
 *   SELECT CASE expr / CASE val[,val] / CASE IS rel / CASE ELSE / END SELECT
 *   FOR     var = expr TO expr [STEP expr]
 *   NEXT    [var]
 *   WHILE   expr … WEND
 *   DO [WHILE|UNTIL expr] … LOOP [WHILE|UNTIL expr]
 *   GOTO    line
 *   GOSUB   line  /  RETURN
 *   ON expr GOTO line[,line,...]
 *   ON expr GOSUB line[,line,...]
 *   READ var / DATA val[,val,...] / RESTORE
 *   DIM var(n)                              (1-D integer arrays, max 256 elem)
 *   CONST name = expr
 *   SWAP    var, var
 *   REM / '  (comment)
 *   END / STOP
 *   CLS
 *   LOCATE  row, col                        (1-based, ANSI)
 *   COLOR   fg [, bg]
 *   BEEP
 *   SLEEP   seconds
 *
 * Functions (numeric):
 *   ABS(x)  INT(x)  SGN(x)  SQR(x)  RND[(x)]  FIX(x)
 *   ASC(s$) VAL(s$) LEN(s$) INSTR([start,]hay$,needle$)
 *
 * Functions (string):
 *   CHR$(n)  STR$(n)  HEX$(n)  OCT$(n)
 *   LEFT$(s$,n)  RIGHT$(s$,n)  MID$(s$,start[,len])
 *   LTRIM$(s$)   RTRIM$(s$)    TRIM$(s$)
 *   UCASE$(s$)   LCASE$(s$)
 *   SPACE$(n)    STRING$(n,c)
 *   INKEY$
 *
 * Variables:
 *   A–Z          integer (int32, suffix %)
 *   A$–Z$        string  (max 127 chars)
 *   A(n)–Z(n)    integer array (DIM required, max 256 elements)
 *
 * Multi-statement lines: colon (:) separator
 * Immediate mode: type without a line number
 * ========================================================================== */

#include "basic.h"
#include "kio.h"
#include "uart.h"
#include "types.h"

/* Tunables */

#define MAX_LINES      512
#define MAX_LINE_LEN   160
#define CALL_DEPTH      32
#define MAX_STR_LEN    127
#define MAX_ARRAY_LEN  256
#define MAX_DATA_ITEMS 256
#define MAX_CONSTS      32

/* RNG state (xorshift32) */
static uint32_t rng_state = 12345;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Program storage */

typedef struct { int num; char text[MAX_LINE_LEN]; } bline_t;
static bline_t prog[MAX_LINES];
static int     prog_count = 0;

/* Variables */

static int32_t int_vars[26];                        /* A%–Z%  */
static char    str_vars[26][MAX_STR_LEN + 1];       /* A$–Z$  */

/* Arrays: one per letter, allocated on DIM */
static int32_t *arr_vars[26];
static int      arr_size[26];

/* Constants table */
typedef struct { char name[16]; int32_t val; } bconst_t;
static bconst_t consts[MAX_CONSTS];
static int      const_count = 0;

/* Call / loop stacks */

typedef struct { int ret_idx; int ret_stmt; } gosub_frame_t;
static gosub_frame_t call_stack[CALL_DEPTH];
static int           call_top = 0;

typedef struct {
    int     var;          /* 0–25 */
    int32_t limit;
    int32_t step;
    int     for_idx;      /* prog[] index of FOR line      */
    int     for_stmt;     /* stmt index within FOR line    */
} for_frame_t;
static for_frame_t for_stack[CALL_DEPTH];
static int         for_top = 0;

typedef struct {
    int     kind;         /* 0=WHILE, 1=DO_WHILE, 2=DO_UNTIL, 3=DO_BARE */
    int     loop_idx;
    int     loop_stmt;
} while_frame_t;
static while_frame_t while_stack[CALL_DEPTH];
static int           while_top = 0;

/* IF block stack */
typedef struct {
    int active;          /* are we currently in the true branch? */
    int done;            /* has a true branch already been taken? */
    int else_seen;
} if_frame_t;
static if_frame_t if_stack[CALL_DEPTH];
static int        if_top = 0;

/* SELECT CASE stack */
typedef struct {
    int32_t val;          /* the expression value */
    int     matched;      /* already matched a CASE? */
    int     done;
} sel_frame_t;
static sel_frame_t sel_stack[CALL_DEPTH];
static int         sel_top = 0;

/* DATA / READ / RESTORE */

static char  data_items[MAX_DATA_ITEMS][32];
static int   data_count = 0;
static int   data_ptr   = 0;

/* Interpreter state */

static bool running  = false;
static bool stopped  = false;
static bool bye_flag = false;

/* Current execution position */
static int cur_idx  = 0;   /* prog[] index   */
static int cur_stmt = 0;   /* colon-stmt idx */

/* Parse pointer */

static const char *gp;     /* global parse pointer */

/* ══════════════════════════════════════════════════════════════════════════
   STRING UTILITIES  (bare-metal, no libc)
   ══════════════════════════════════════════════════════════════════════════ */

static int bstrlen(const char *s) { int i=0; while(s[i]) i++; return i; }

static void bstrcpy(char *d, const char *s) { while((*d++=*s++)); }

static void bstrncpy(char *d, const char *s, int n)
{
    int i=0;
    while(i<n-1 && s[i]) { d[i]=s[i]; i++; }
    d[i]='\0';
}

static int bstrcmp(const char *a, const char *b)
{
    while(*a && *a==*b){a++;b++;}
    return (unsigned char)*a-(unsigned char)*b;
}

static int bstrncmp(const char *a, const char *b, int n)
{
    while(n-- && *a && *a==*b){a++;b++;}
    if(n<0) return 0;
    return (unsigned char)*a-(unsigned char)*b;
}

/* Output helpers */

static void bputc(char c) { uart_putc(c); }

static void bputs(const char *s) {
    while(*s){ if(*s=='\n'){uart_putc('\r');} uart_putc(*s++); }
}

static void bputi(int32_t v) { kprintf("%d", (int)v); }

static void bputnl(void) { uart_putc('\r'); uart_putc('\n'); }

/* ANSI helpers */

static void ansi_cls(void)     { bputs("\x1b[2J\x1b[H"); }
static void ansi_locate(int r, int c) { kprintf("\x1b[%d;%dH", r, c); }
static void ansi_color(int fg, int bg)
{
    if(fg>=0) kprintf("\x1b[%dm", 30 + (fg & 7) + (fg>7?60:0));
    if(bg>=0) kprintf("\x1b[%dm", 40 + (bg & 7));
}
static void ansi_reset(void) { bputs("\x1b[0m"); }

/* Forward declaration for input function */
static void basic_getline(char *buf, int maxlen);

/* ══════════════════════════════════════════════════════════════════════════
   PARSE HELPERS
   ══════════════════════════════════════════════════════════════════════════ */

static void skip_sp(void) { while(*gp==' '||*gp=='\t') gp++; }

/* Match keyword kw (upper-case) at gp, followed by non-alpha */
static bool kw(const char *k)
{
    int n = bstrlen(k);
    if(bstrncmp(gp, k, n) != 0) return false;
    char next = gp[n];
    if((next>='A'&&next<='Z')||(next>='a'&&next<='z')||
       (next>='0'&&next<='9')||next=='_'||next=='$'||next=='%')
        return false;
    gp += n;
    return true;
}

/* Match keyword k, next char may be anything (for multi-char tokens) */
static bool kw_any(const char *k)
{
    int n = bstrlen(k);
    if(bstrncmp(gp, k, n) != 0) return false;
    gp += n;
    return true;
}

/* Skip statements */

static void skip_to_colon(void)
{
    while(*gp && *gp!=':') {
        if(*gp=='"') { gp++; while(*gp&&*gp!='"') gp++; if(*gp) gp++; }
        else gp++;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   EXPRESSION EVALUATOR  (integer + string)
   ══════════════════════════════════════════════════════════════════════════ */

static int32_t parse_expr(void);
static void    parse_str_expr(char *out, int maxlen);

/* Integer functions */

static int32_t fn_sqr(void)
{
    int32_t v = parse_expr();
    if(v<0) v=0;
    /* integer sqrt via Newton */
    if(v==0) return 0;
    int32_t x=v, y=(x+1)/2;
    while(y<x){ x=y; y=(y+v/y)/2; }
    return x;
}
static int32_t fn_rnd(void)
{
    /* optional arg in () — consume it */
    skip_sp();
    if(*gp=='(') { gp++; parse_expr(); skip_sp(); if(*gp==')') gp++; }
    return (int32_t)(rng_next() % 32768);
}
static int32_t fn_val(void)
{
    char buf[MAX_STR_LEN+1];
    skip_sp(); if(*gp=='(') gp++;
    parse_str_expr(buf, sizeof(buf));
    skip_sp(); if(*gp==')') gp++;
    return katoi(buf);
}
static int32_t fn_len(void)
{
    char buf[MAX_STR_LEN+1];
    skip_sp(); if(*gp=='(') gp++;
    parse_str_expr(buf, sizeof(buf));
    skip_sp(); if(*gp==')') gp++;
    return bstrlen(buf);
}
static int32_t fn_asc(void)
{
    char buf[MAX_STR_LEN+1];
    skip_sp(); if(*gp=='(') gp++;
    parse_str_expr(buf, sizeof(buf));
    skip_sp(); if(*gp==')') gp++;
    return buf[0] ? (unsigned char)buf[0] : 0;
}
static int32_t fn_instr(void)
{
    /* INSTR([start,] hay$, needle$) */
    skip_sp(); if(*gp=='(') gp++;
    /* peek: if first arg is numeric, it's start */
    int32_t start = 1;
    /* Treat a numeric first argument as the optional start position. */
    const char *save = gp;
    skip_sp();
    bool first_is_num = false;
    if((*gp>='0'&&*gp<='9')||*gp=='-') first_is_num=true;
    else if(*gp>='A'&&*gp<='Z'&&*(gp+1)!='$') first_is_num=true;
    if(first_is_num) {
        start = parse_expr();
        skip_sp(); if(*gp==',') gp++; else { gp=save; start=1; }
    }
    char hay[MAX_STR_LEN+1], needle[MAX_STR_LEN+1];
    parse_str_expr(hay, sizeof(hay));
    skip_sp(); if(*gp==',') gp++;
    parse_str_expr(needle, sizeof(needle));
    skip_sp(); if(*gp==')') gp++;
    int hlen=bstrlen(hay), nlen=bstrlen(needle);
    if(nlen==0) return start;
    for(int i=start-1; i<=hlen-nlen; i++) {
        if(bstrncmp(hay+i,needle,nlen)==0) return i+1;
    }
    return 0;
}

/* String functions */

static void fn_chr(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    int32_t v = parse_expr();
    skip_sp(); if(*gp==')') gp++;
    out[0]=(char)(v&0xFF); out[1]='\0';
}
static void fn_str(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    int32_t v = parse_expr();
    skip_sp(); if(*gp==')') gp++;
    kstrncpy(out, " ", MAX_STR_LEN);   /* QB adds leading space for positive */
    if(v<0){ out[0]='-'; kprintf(""); /* use kio */ }
    /* manual int-to-string */
    char tmp[20]; int i=0;
    bool neg=(v<0);
    if(neg) v=-v;
    if(v==0){out[neg?1:1]='0';out[neg?2:2]='\0';}
    else{
        while(v){tmp[i++]='0'+v%10;v/=10;}
        int base=neg?1:1;
        for(int j=i-1;j>=0;j--) out[base++]=tmp[j];
        out[base]='\0';
    }
}
static void fn_hex(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    uint32_t v = (uint32_t)parse_expr();
    skip_sp(); if(*gp==')') gp++;
    const char h[]="0123456789ABCDEF";
    char tmp[12]; int i=0;
    if(v==0){out[0]='0';out[1]='\0';return;}
    while(v){tmp[i++]=h[v%16];v/=16;}
    for(int j=i-1;j>=0;j--) out[i-1-j]=tmp[j];
    out[i]='\0';
}
static void fn_oct(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    uint32_t v=(uint32_t)parse_expr();
    skip_sp(); if(*gp==')') gp++;
    char tmp[16]; int i=0;
    if(v==0){out[0]='0';out[1]='\0';return;}
    while(v){tmp[i++]='0'+v%8;v/=8;}
    for(int j=i-1;j>=0;j--) out[i-1-j]=tmp[j];
    out[i]='\0';
}
static void fn_left(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    char s[MAX_STR_LEN+1]; parse_str_expr(s,sizeof(s));
    skip_sp(); if(*gp==',') gp++;
    int32_t n=parse_expr();
    skip_sp(); if(*gp==')') gp++;
    int len=bstrlen(s); if(n>len)n=len;
    bstrncpy(out,s,n+1); out[n]='\0';
}
static void fn_right(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    char s[MAX_STR_LEN+1]; parse_str_expr(s,sizeof(s));
    skip_sp(); if(*gp==',') gp++;
    int32_t n=parse_expr();
    skip_sp(); if(*gp==')') gp++;
    int len=bstrlen(s); if(n>len)n=len;
    bstrcpy(out, s+len-n);
}
static void fn_mid(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    char s[MAX_STR_LEN+1]; parse_str_expr(s,sizeof(s));
    skip_sp(); if(*gp==',') gp++;
    int32_t start=parse_expr()-1; if(start<0)start=0;
    int32_t len=-1;
    skip_sp(); if(*gp==','){gp++;len=parse_expr();}
    skip_sp(); if(*gp==')') gp++;
    int slen=bstrlen(s);
    if(start>=slen){out[0]='\0';return;}
    if(len<0||start+len>slen) len=slen-start;
    bstrncpy(out,s+start,len+1); out[len]='\0';
}
static void fn_ltrim(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    char s[MAX_STR_LEN+1]; parse_str_expr(s,sizeof(s));
    skip_sp(); if(*gp==')') gp++;
    const char *p=s; while(*p==' ')p++;
    bstrcpy(out,p);
}
static void fn_rtrim(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    char s[MAX_STR_LEN+1]; parse_str_expr(s,sizeof(s));
    skip_sp(); if(*gp==')') gp++;
    bstrcpy(out,s);
    int i=bstrlen(out)-1; while(i>=0&&out[i]==' ')out[i--]='\0';
}
static void fn_trim(char *out)
{
    char tmp[MAX_STR_LEN+1]; fn_ltrim(tmp);
    bstrcpy(out,tmp);
    int i=bstrlen(out)-1; while(i>=0&&out[i]==' ')out[i--]='\0';
}
static void fn_ucase(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    char s[MAX_STR_LEN+1]; parse_str_expr(s,sizeof(s));
    skip_sp(); if(*gp==')') gp++;
    for(int i=0;s[i];i++) out[i]=(s[i]>='a'&&s[i]<='z')?s[i]-32:s[i];
    out[bstrlen(s)]='\0';
}
static void fn_lcase(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    char s[MAX_STR_LEN+1]; parse_str_expr(s,sizeof(s));
    skip_sp(); if(*gp==')') gp++;
    for(int i=0;s[i];i++) out[i]=(s[i]>='A'&&s[i]<='Z')?s[i]+32:s[i];
    out[bstrlen(s)]='\0';
}
static void fn_space(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    int32_t n=parse_expr();
    skip_sp(); if(*gp==')') gp++;
    if(n>MAX_STR_LEN) n=MAX_STR_LEN;
    for(int i=0;i<(int)n;i++) out[i]=' ';
    out[n]='\0';
}
static void fn_string(char *out)
{
    skip_sp(); if(*gp=='(') gp++;
    int32_t n=parse_expr();
    skip_sp(); if(*gp==',') gp++;
    char buf[MAX_STR_LEN+1]; parse_str_expr(buf,sizeof(buf));
    skip_sp(); if(*gp==')') gp++;
    char ch=buf[0]?buf[0]:' ';
    if(n>MAX_STR_LEN) n=MAX_STR_LEN;
    for(int i=0;i<(int)n;i++) out[i]=ch;
    out[n]='\0';
}
static void fn_inkey(char *out)
{
    if(uart_ready()) {
        out[0]=uart_getc(); out[1]='\0';
    } else {
        out[0]='\0';
    }
}

/* String expression parser */

static void parse_str_expr(char *out, int maxlen)
{
    char tmp[MAX_STR_LEN+1];
    out[0]='\0';
    skip_sp();

    /* first operand */
    auto_again:
    tmp[0]='\0';
    skip_sp();

    if(*gp=='"') {
        /* string literal */
        gp++;
        int i=0;
        while(*gp&&*gp!='"'&&i<MAX_STR_LEN) tmp[i++]=*gp++;
        tmp[i]='\0';
        if(*gp=='"') gp++;
    } else if(*gp>='A'&&*gp<='Z'&&*(gp+1)=='$') {
        /* string variable */
        int idx=*gp-'A'; gp+=2;
        bstrcpy(tmp, str_vars[idx]);
    } else if(kw("CHR$"))    { fn_chr(tmp); }
    else if(kw("STR$"))      { fn_str(tmp); }
    else if(kw("HEX$"))      { fn_hex(tmp); }
    else if(kw("OCT$"))      { fn_oct(tmp); }
    else if(kw("LEFT$"))     { fn_left(tmp); }
    else if(kw("RIGHT$"))    { fn_right(tmp); }
    else if(kw("MID$"))      { fn_mid(tmp); }
    else if(kw("LTRIM$"))    { fn_ltrim(tmp); }
    else if(kw("RTRIM$"))    { fn_rtrim(tmp); }
    else if(kw("TRIM$"))     { fn_trim(tmp); }
    else if(kw("UCASE$"))    { fn_ucase(tmp); }
    else if(kw("LCASE$"))    { fn_lcase(tmp); }
    else if(kw("SPACE$"))    { fn_space(tmp); }
    else if(kw("STRING$"))   { fn_string(tmp); }
    else if(kw("INKEY$"))    { fn_inkey(tmp); }
    else {
        /* number coercion: convert integer expression to string */
        int32_t v=parse_expr();
        /* convert to string */
        char nbuf[20]; int ni=0;
        bool neg=(v<0);
        if(neg) v=-v;
        if(v==0){nbuf[ni++]='0';}
        else{char t[12];int ti=0;while(v){t[ti++]='0'+v%10;v/=10;}
             while(ti--)nbuf[ni++]=t[ti];}
        if(neg){/* shift right and prepend - */
            for(int i=ni;i>0;i--)nbuf[i]=nbuf[i-1];nbuf[0]='-';ni++;}
        nbuf[ni]='\0';
        bstrcpy(tmp,nbuf);
    }

    /* concatenate into out */
    int olen=bstrlen(out), tlen=bstrlen(tmp);
    if(olen+tlen<maxlen-1) {
        bstrcpy(out+olen, tmp);
    }

    /* check for + concatenation */
    skip_sp();
    if(*gp=='+') {
        /* Peek: only concat if right side is string */
        const char *peek=gp+1;
        while(*peek==' ') peek++;
        bool rhs_str = (*peek=='"') ||
                       (*peek>='A'&&*peek<='Z'&&*(peek+1)=='$') ||
                       (bstrncmp(peek,"CHR$",4)==0) ||
                       (bstrncmp(peek,"STR$",4)==0) ||
                       (bstrncmp(peek,"LEFT$",5)==0) ||
                       (bstrncmp(peek,"RIGHT$",6)==0) ||
                       (bstrncmp(peek,"MID$",4)==0) ||
                       (bstrncmp(peek,"UCASE$",6)==0) ||
                       (bstrncmp(peek,"LCASE$",6)==0) ||
                       (bstrncmp(peek,"SPACE$",6)==0) ||
                       (bstrncmp(peek,"INKEY$",6)==0) ||
                       (bstrncmp(peek,"HEX$",4)==0);
        if(rhs_str) { gp++; goto auto_again; }
    }
}

/* Primary parser */

static int32_t parse_primary(void)
{
    skip_sp();
    /* Unary */
    if(*gp=='(') { gp++; int32_t v=parse_expr(); skip_sp(); if(*gp==')')gp++; return v; }
    if(*gp=='-') { gp++; return -parse_primary(); }
    if(*gp=='+') { gp++; return  parse_primary(); }
    if(kw("NOT")) { return ~parse_primary(); }

    /* Functions */
    if(kw("ABS"))   { skip_sp();if(*gp=='(')gp++; int32_t v=parse_expr(); skip_sp();if(*gp==')')gp++; return v<0?-v:v; }
    if(kw("SGN"))   { skip_sp();if(*gp=='(')gp++; int32_t v=parse_expr(); skip_sp();if(*gp==')')gp++; return v>0?1:(v<0?-1:0); }
    if(kw("INT"))   { skip_sp();if(*gp=='(')gp++; int32_t v=parse_expr(); skip_sp();if(*gp==')')gp++; return v; }
    if(kw("FIX"))   { skip_sp();if(*gp=='(')gp++; int32_t v=parse_expr(); skip_sp();if(*gp==')')gp++; return v; }
    if(kw("SQR"))   { return fn_sqr(); }
    if(kw("RND"))   { return fn_rnd(); }
    if(kw("VAL"))   { return fn_val(); }
    if(kw("LEN"))   { return fn_len(); }
    if(kw("ASC"))   { return fn_asc(); }
    if(kw("INSTR")) { return fn_instr(); }

    /* Array variable A(n) */
    if(*gp>='A'&&*gp<='Z'&&*(gp+1)=='('&&*(gp+2)!='$') {
        int idx=*gp-'A'; gp+=2;
        int32_t subscr=parse_expr();
        skip_sp(); if(*gp==')') gp++;
        if(!arr_vars[idx]||subscr<0||subscr>=arr_size[idx]) {
            kprintf("?Subscript out of range\n"); return 0;
        }
        return arr_vars[idx][subscr];
    }

    /* Integer variable A–Z (or A% style) */
    if(*gp>='A'&&*gp<='Z') {
        /* Check it's not a string var or array */
        if(*(gp+1)!='$'&&*(gp+1)!='(') {
            int idx=*gp-'A'; gp++;
            if(*gp=='%') gp++;   /* optional type suffix */
            /* Check constants table */
            char name[2]={(char)('A'+idx),'\0'};
            for(int i=0;i<const_count;i++) {
                if(bstrcmp(consts[i].name,name)==0) return consts[i].val;
            }
            return int_vars[idx];
        }
    }

    /* Hex literal &H */
    if(*gp=='&'&&*(gp+1)=='H') {
        gp+=2;
        int32_t v=0;
        while((*gp>='0'&&*gp<='9')||(*gp>='A'&&*gp<='F')||(*gp>='a'&&*gp<='f')) {
            int d=(*gp>='a')?*gp-'a'+10:(*gp>='A')?*gp-'A'+10:*gp-'0';
            v=v*16+d; gp++;
        }
        return v;
    }

    /* Octal literal &O */
    if(*gp=='&'&&*(gp+1)=='O') {
        gp+=2;
        int32_t v=0;
        while(*gp>='0'&&*gp<='7'){v=v*8+(*gp++-'0');}
        return v;
    }

    /* Decimal literal */
    int32_t v=0; bool got=false;
    while(*gp>='0'&&*gp<='9'){v=v*10+(*gp++-'0');got=true;}
    if(*gp=='.') { gp++; while(*gp>='0'&&*gp<='9') gp++; } /* skip frac */
    if(!got) return 0;
    return v;
}

static int32_t parse_power(void)
{
    int32_t base=parse_primary();
    skip_sp();
    if(*gp=='^') {
        gp++;
        int32_t exp=parse_primary();
        int32_t r=1;
        for(int32_t i=0;i<(exp<0?0:exp);i++) r*=base;
        return r;
    }
    return base;
}

static int32_t parse_term(void)
{
    int32_t v=parse_power();
    skip_sp();
    while(*gp=='*'||*gp=='/'||bstrncmp(gp,"MOD",3)==0||bstrncmp(gp,"\\",1)==0) {
        if(bstrncmp(gp,"MOD",3)==0){gp+=3;int32_t r=parse_power();v=r?v%r:0;}
        else if(*gp=='\\'){gp++;int32_t d=parse_power();v=d?v/d:0;} /* integer div */
        else if(*gp=='*'){gp++;v*=parse_power();}
        else{gp++;int32_t d=parse_power();v=d?v/d:0;}
        skip_sp();
    }
    return v;
}

static int32_t parse_add(void)
{
    int32_t v=parse_term();
    skip_sp();
    while(*gp=='+'||*gp=='-') {
        if(*gp=='+'){gp++;v+=parse_term();}
        else{gp++;v-=parse_term();}
        skip_sp();
    }
    return v;
}

static int32_t parse_rel(void)
{
    int32_t lhs=parse_add();
    skip_sp();
    if(kw_any("<>")){ return lhs!=parse_add()?-1:0; }
    if(kw_any("<=")){ return lhs<=parse_add()?-1:0; }
    if(kw_any(">=")){ return lhs>=parse_add()?-1:0; }
    if(*gp=='<'){gp++;return lhs< parse_add()?-1:0;}
    if(*gp=='>'){gp++;return lhs> parse_add()?-1:0;}
    if(*gp=='='){gp++;return lhs==parse_add()?-1:0;}
    return lhs;
}

static int32_t parse_not(void)
{
    if(kw("NOT")) return ~parse_not();
    return parse_rel();
}

static int32_t parse_and(void)
{
    int32_t v=parse_not();
    skip_sp();
    while(kw("AND")){v&=parse_not();skip_sp();}
    return v;
}

static int32_t parse_or(void)
{
    int32_t v=parse_and();
    skip_sp();
    while(kw("OR")){v|=parse_and();skip_sp();}
    return v;
}

static int32_t parse_xor(void)
{
    int32_t v=parse_or();
    skip_sp();
    while(kw("XOR")){v^=parse_or();skip_sp();}
    return v;
}

static int32_t parse_expr(void) { return parse_xor(); }

/* ══════════════════════════════════════════════════════════════════════════
   LINE MANAGEMENT
   ══════════════════════════════════════════════════════════════════════════ */

static int find_line_idx(int num)
{
    for(int i=0;i<prog_count;i++) if(prog[i].num==num) return i;
    return -1;
}

static void insert_line(int num, const char *text)
{
    int idx=find_line_idx(num);
    if(idx>=0) {
        if(!text[0]) {
            for(int i=idx;i<prog_count-1;i++) prog[i]=prog[i+1];
            prog_count--;
        } else {
            bstrncpy(prog[idx].text, text, MAX_LINE_LEN);
        }
        return;
    }
    if(prog_count>=MAX_LINES){kprintf("?Program too large\n");return;}
    int ins=prog_count;
    for(int i=0;i<prog_count;i++){if(prog[i].num>num){ins=i;break;}}
    for(int i=prog_count;i>ins;i--) prog[i]=prog[i-1];
    prog[ins].num=num;
    bstrncpy(prog[ins].text, text, MAX_LINE_LEN);
    prog_count++;
}

/* DATA scanner */

static void collect_data(void)
{
    data_count=0; data_ptr=0;
    for(int i=0;i<prog_count&&data_count<MAX_DATA_ITEMS;i++) {
        const char *p=prog[i].text;
        /* skip line number */
        while(*p>='0'&&*p<='9') p++;
        while(*p==' ') p++;
        if(bstrncmp(p,"DATA",4)==0&&(p[4]==' '||p[4]=='\0')) {
            p+=4; while(*p==' ')p++;
            while(*p&&data_count<MAX_DATA_ITEMS) {
                /* read one item */
                int di=0;
                if(*p=='"'){
                    p++;
                    while(*p&&*p!='"'&&di<31) data_items[data_count][di++]=*p++;
                    if(*p=='"') p++;
                } else {
                    while(*p&&*p!=','&&di<31) data_items[data_count][di++]=*p++;
                    /* trim trailing spaces */
                    while(di>0&&data_items[data_count][di-1]==' ') di--;
                }
                data_items[data_count][di]='\0';
                data_count++;
                while(*p==' ')p++;
                if(*p==','){p++;while(*p==' ')p++;}
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   STATEMENT EXECUTOR
   ══════════════════════════════════════════════════════════════════════════ */

/* Split a line into colon-separated statements.
   Returns pointer to stmt #n within line text (skip line number first). */
static const char *get_stmt(const char *text, int n)
{
    const char *p=text;
    while(*p>='0'&&*p<='9') p++;   /* skip line number */
    while(*p==' ') p++;
    int s=0;
    while(s<n) {
        while(*p&&*p!=':') {
            if(*p=='"'){p++;while(*p&&*p!='"')p++;if(*p)p++;}
            else p++;
        }
        if(!*p) return NULL;
        p++; s++;
        while(*p==' ') p++;
    }
    return p;
}

/* Count colon-separated statements in a line */
static int count_stmts(const char *text)
{
    const char *p=text;
    while(*p>='0'&&*p<='9') p++;
    while(*p==' ') p++;
    int n=1;
    while(*p) {
        if(*p=='"'){p++;while(*p&&*p!='"')p++;if(*p)p++;}
        else if(*p==':'){n++;p++;}
        else p++;
    }
    return n;
}

/* Advance to next statement; return false if line exhausted */
static bool advance_stmt(void)
{
    int ns=count_stmts(prog[cur_idx].text);
    if(cur_stmt+1<ns){ cur_stmt++; return true; }
    return false;
}

/* Jump to given prog[] index, stmt 0 */
static void jump_to(int idx)
{
    cur_idx=idx; cur_stmt=0;
}

/* PRINT */

static void exec_print(void)
{
    bool need_nl=true;
    for(;;) {
        skip_sp();
        if(*gp=='\0'||*gp==':') break;

        /* TAB(n) */
        if(kw("TAB")) {
            skip_sp(); if(*gp=='(') gp++;
            int32_t col=parse_expr(); skip_sp(); if(*gp==')') gp++;
            for(int i=0;i<(int)col;i++) bputc(' ');
            skip_sp();
            if(*gp==';'){gp++;need_nl=false;} else break;
            continue;
        }
        /* SPC(n) */
        if(kw("SPC")) {
            skip_sp(); if(*gp=='(') gp++;
            int32_t n=parse_expr(); skip_sp(); if(*gp==')') gp++;
            for(int i=0;i<(int)n;i++) bputc(' ');
            skip_sp();
            if(*gp==';'){gp++;need_nl=false;} else break;
            continue;
        }
        /* String expression? */
        bool is_str=false;
        if(*gp=='"') is_str=true;
        else if(*gp>='A'&&*gp<='Z'&&*(gp+1)=='$') is_str=true;
        else {
            /* check string functions */
            const char *sfns[]={"CHR$","STR$","HEX$","OCT$","LEFT$","RIGHT$",
                                 "MID$","UCASE$","LCASE$","SPACE$","STRING$",
                                 "LTRIM$","RTRIM$","TRIM$","INKEY$",NULL};
            for(int i=0;sfns[i];i++) {
                if(bstrncmp(gp,sfns[i],bstrlen(sfns[i]))==0){is_str=true;break;}
            }
        }
        if(is_str) {
            char buf[MAX_STR_LEN+1];
            parse_str_expr(buf, sizeof(buf));
            bputs(buf);
        } else {
            int32_t v=parse_expr();
            if(v>=0) bputc(' ');
            bputi(v);
            bputc(' ');
        }
        need_nl=true;
        skip_sp();
        if(*gp==';'){gp++;need_nl=false;}
        else if(*gp==','){gp++;bputc('\t');need_nl=false;}
        else break;
    }
    if(need_nl) bputnl();
}

/* LET */

static void exec_let(void)
{
    skip_sp();
    if(*gp<'A'||*gp>'Z'){kprintf("?Syntax\n");return;}
    int var=*gp++-'A';

    /* String variable */
    if(*gp=='$') {
        gp++;
        skip_sp(); if(*gp=='=') gp++;
        skip_sp();
        parse_str_expr(str_vars[var], MAX_STR_LEN+1);
        return;
    }

    /* Array variable */
    if(*gp=='(') {
        gp++;
        int32_t subscr=parse_expr();
        skip_sp(); if(*gp==')') gp++;
        skip_sp(); if(*gp=='=') gp++;
        skip_sp();
        if(!arr_vars[var]||subscr<0||subscr>=arr_size[var]) {
            kprintf("?Subscript out of range\n"); return;
        }
        arr_vars[var][subscr]=parse_expr();
        return;
    }

    if(*gp=='%') gp++;   /* optional suffix */
    skip_sp(); if(*gp=='=') gp++;
    skip_sp();
    int_vars[var]=parse_expr();
}

/* INPUT */

static void exec_input(void)
{
    skip_sp();
    /* Optional prompt: INPUT "str"; var  or  INPUT "str", var */
    if(*gp=='"') {
        gp++;
        while(*gp&&*gp!='"') bputc(*gp++);
        if(*gp=='"') gp++;
        skip_sp();
        if(*gp==';'||*gp==',') gp++;
    }
    skip_sp();
    if(*gp<'A'||*gp>'Z'){kprintf("?Syntax\n");return;}
    int var=*gp++-'A';
    bool is_str=(*gp=='$');
    if(is_str) gp++;

    bputc('?'); bputc(' ');
    static char ibuf[MAX_STR_LEN+1];
    kgets(ibuf, sizeof(ibuf));

    if(is_str) {
        bstrncpy(str_vars[var], ibuf, MAX_STR_LEN+1);
    } else {
        int_vars[var]=katoi(ibuf);
    }
}

/* GOTO / GOSUB */

static void exec_goto(void)
{
    skip_sp();
    int linenum=(int)parse_expr();
    int idx=find_line_idx(linenum);
    if(idx<0){kprintf("?Undefined line %d\n",linenum);stopped=true;return;}
    jump_to(idx);
}

static void exec_gosub(void)
{
    skip_sp();
    int linenum=(int)parse_expr();
    int idx=find_line_idx(linenum);
    if(idx<0){kprintf("?Undefined line %d\n",linenum);stopped=true;return;}
    if(call_top>=CALL_DEPTH){kprintf("?GOSUB overflow\n");stopped=true;return;}
    call_stack[call_top].ret_idx=cur_idx;
    call_stack[call_top].ret_stmt=cur_stmt+1;
    call_top++;
    jump_to(idx);
}

static void exec_return(void)
{
    if(call_top<=0){kprintf("?RETURN without GOSUB\n");stopped=true;return;}
    call_top--;
    cur_idx=call_stack[call_top].ret_idx;
    cur_stmt=call_stack[call_top].ret_stmt;
    /* if stmt exhausted, move to next line */
    if(cur_stmt>=count_stmts(prog[cur_idx].text)){
        cur_idx++; cur_stmt=0;
    }
}

/* FOR / NEXT */

static void exec_for(void)
{
    if(for_top>=CALL_DEPTH){kprintf("?FOR stack overflow\n");stopped=true;return;}
    skip_sp();
    if(*gp<'A'||*gp>'Z'){kprintf("?Syntax\n");stopped=true;return;}
    int var=*gp++-'A';
    skip_sp(); if(*gp=='=') gp++;
    int32_t from=parse_expr();
    skip_sp(); kw("TO");
    int32_t limit=parse_expr();
    int32_t step=1;
    skip_sp(); if(kw("STEP")) step=parse_expr();

    int_vars[var]=from;
    for_stack[for_top].var=var;
    for_stack[for_top].limit=limit;
    for_stack[for_top].step=step;
    for_stack[for_top].for_idx=cur_idx;
    for_stack[for_top].for_stmt=cur_stmt;
    for_top++;
    /* continue to body */
}

static void exec_next(void)
{
    skip_sp();
    int var=-1;
    if(*gp>='A'&&*gp<='Z'&&*(gp+1)!='$'&&*(gp+1)!='(') {
        var=*gp++-'A'; if(*gp=='%')gp++;
    }
    if(for_top<=0){kprintf("?NEXT without FOR\n");stopped=true;return;}
    for_frame_t *f=&for_stack[for_top-1];
    if(var>=0&&f->var!=var){kprintf("?NEXT variable mismatch\n");stopped=true;return;}
    int_vars[f->var]+=f->step;
    bool done=(f->step>=0)?(int_vars[f->var]>f->limit):(int_vars[f->var]<f->limit);
    if(done) {
        for_top--;
    } else {
        /* jump back to statement after FOR */
        cur_idx=f->for_idx;
        cur_stmt=f->for_stmt+1;
        if(cur_stmt>=count_stmts(prog[cur_idx].text)){
            cur_idx++; cur_stmt=0;
        }
    }
}

/* WHILE / WEND */

static void exec_while(void)
{
    if(while_top>=CALL_DEPTH){kprintf("?WHILE stack overflow\n");stopped=true;return;}
    int32_t cond=parse_expr();
    if(cond) {
        while_stack[while_top].kind=0;
        while_stack[while_top].loop_idx=cur_idx;
        while_stack[while_top].loop_stmt=cur_stmt;
        while_top++;
    } else {
        /* Skip to matching WEND */
        int depth=1;
        while(cur_idx<prog_count&&depth>0) {
            if(!advance_stmt()){cur_idx++;cur_stmt=0;if(cur_idx>=prog_count)break;}
            const char *p=get_stmt(prog[cur_idx].text, cur_stmt);
            if(!p) continue;
            while(*p==' ')p++;
            if(bstrncmp(p,"WHILE",5)==0) depth++;
            else if(bstrncmp(p,"WEND",4)==0) depth--;
        }
    }
}

static void exec_wend(void)
{
    if(while_top<=0){kprintf("?WEND without WHILE\n");stopped=true;return;}
    while_frame_t *f=&while_stack[while_top-1];
    /* re-evaluate condition */
    const char *save=gp;
    cur_idx=f->loop_idx; cur_stmt=f->loop_stmt;
    const char *p=get_stmt(prog[cur_idx].text,cur_stmt);
    if(!p){while_top--;return;}
    while(*p==' ')p++;
    if(bstrncmp(p,"WHILE",5)==0) p+=5;
    gp=p;
    int32_t cond=parse_expr();
    (void)save;
    if(cond) {
        /* stay in loop, advance past WHILE statement */
        if(!advance_stmt()){cur_idx++;cur_stmt=0;}
    } else {
        while_top--;
        /* cur_idx/cur_stmt already past WEND, will be advanced by main loop */
    }
}

/* DO / LOOP */

static void exec_do(void)
{
    if(while_top>=CALL_DEPTH){kprintf("?DO stack overflow\n");stopped=true;return;}
    skip_sp();
    int kind=3; /* bare DO */
    int32_t cond=1;
    if(kw("WHILE")){kind=1;cond=parse_expr();}
    else if(kw("UNTIL")){kind=2;cond=!parse_expr();}

    if(cond) {
        while_stack[while_top].kind=kind;
        while_stack[while_top].loop_idx=cur_idx;
        while_stack[while_top].loop_stmt=cur_stmt;
        while_top++;
    } else {
        /* skip to LOOP */
        int depth=1;
        while(cur_idx<prog_count&&depth>0) {
            if(!advance_stmt()){cur_idx++;cur_stmt=0;if(cur_idx>=prog_count)break;}
            const char *p=get_stmt(prog[cur_idx].text,cur_stmt);
            if(!p)continue;
            while(*p==' ')p++;
            if(bstrncmp(p,"DO",2)==0&&(p[2]==' '||p[2]=='\0'||p[2]=='\r')) depth++;
            else if(bstrncmp(p,"LOOP",4)==0) depth--;
        }
    }
}

static void exec_loop(void)
{
    skip_sp();
    if(while_top<=0){kprintf("?LOOP without DO\n");stopped=true;return;}
    while_frame_t *f=&while_stack[while_top-1];
    int32_t cond=1;
    if(kw("WHILE"))  cond= parse_expr();
    else if(kw("UNTIL")) cond=!parse_expr();

    if(cond) {
        /* jump back to DO body */
        cur_idx=f->loop_idx; cur_stmt=f->loop_stmt;
        if(!advance_stmt()){cur_idx++;cur_stmt=0;}
    } else {
        while_top--;
        /* fall through to next stmt */
    }
}

/* IF (single-line and block) */

/* Single-line: IF cond THEN stmts [:] [ELSE stmts] */
static void exec_if_single(int32_t cond)
{
    /* Find the top-level ELSE, ignoring strings and nested IFs. */
    const char *else_pos=NULL;
    const char *scan=gp;
    int depth=0;
    while(*scan) {
        if(*scan=='"'){scan++;while(*scan&&*scan!='"')scan++;if(*scan)scan++;continue;}
        if(bstrncmp(scan,"IF",2)==0&&(scan[2]==' '||scan[2]=='(')) depth++;
        if(bstrncmp(scan,"END IF",6)==0) depth--;
        if(depth==0&&bstrncmp(scan,"ELSE",4)==0&&
           (scan[4]==' '||scan[4]=='\0'||scan[4]=='\r')) { else_pos=scan; break; }
        scan++;
    }

    if(cond) {
        /* A line number after THEN is an implicit GOTO. */
        skip_sp();
        if(*gp>='0'&&*gp<='9') {
            int ln=(int)parse_expr();
            int idx=find_line_idx(ln);
            if(idx<0){kprintf("?Undefined line %d\n",ln);stopped=true;}
            else jump_to(idx);
            return;
        }
        /* Leave the THEN statement for exec_line to dispatch. */
        (void)else_pos;
        /* gp stays at THEN body — let exec_line dispatch it */
    } else {
        if(else_pos) {
            gp=else_pos+4; /* skip "ELSE" */
            skip_sp();
        } else {
            /* skip rest of line */
            while(*gp) gp++;
        }
    }
}

/* Block IF */
static void exec_if_block(int32_t cond)
{
    if(if_top>=CALL_DEPTH){kprintf("?IF stack overflow\n");stopped=true;return;}
    if_stack[if_top].active=(cond!=0);
    if_stack[if_top].done=(cond!=0);
    if_stack[if_top].else_seen=0;
    if_top++;
    if(!cond) {
        /* skip to ELSE / ELSEIF / END IF */
        int depth=1;
        while(cur_idx<prog_count&&depth>0) {
            if(!advance_stmt()){cur_idx++;cur_stmt=0;if(cur_idx>=prog_count)break;}
            const char *p=get_stmt(prog[cur_idx].text,cur_stmt);
            if(!p)continue;
            while(*p==' ')p++;
            if(bstrncmp(p,"IF",2)==0&&(p[2]==' '||p[2]=='(')) {
                /* peek: check if there's a THEN at end (block form) */
                /* quick check: scan for THEN with nothing after */
                depth++;
            } else if(bstrncmp(p,"END IF",6)==0) {
                depth--;
                if(depth==0) break;
            } else if(depth==1&&(bstrncmp(p,"ELSE",4)==0||
                                  bstrncmp(p,"ELSEIF",6)==0)) {
                break;  /* handle at ELSE/ELSEIF exec */
            }
        }
    }
}

static void exec_elseif(void)
{
    if(if_top<=0){kprintf("?ELSEIF without IF\n");stopped=true;return;}
    if_frame_t *f=&if_stack[if_top-1];
    if(f->done) {
        /* skip to END IF */
        int depth=1;
        while(cur_idx<prog_count&&depth>0) {
            if(!advance_stmt()){cur_idx++;cur_stmt=0;if(cur_idx>=prog_count)break;}
            const char *p=get_stmt(prog[cur_idx].text,cur_stmt);
            if(!p)continue;
            while(*p==' ')p++;
            if(bstrncmp(p,"IF",2)==0) depth++;
            else if(bstrncmp(p,"END IF",6)==0) { depth--; if(!depth) break; }
        }
        if_top--;
        return;
    }
    int32_t cond=parse_expr();
    skip_sp(); kw("THEN");
    if(cond) {
        f->active=1; f->done=1;
    } else {
        f->active=0;
        exec_if_block(0);   /* reuse block skip logic */
        if_top--;           /* exec_if_block pushed, pop extra */
    }
}

static void exec_else(void)
{
    if(if_top<=0){kprintf("?ELSE without IF\n");stopped=true;return;}
    if_frame_t *f=&if_stack[if_top-1];
    if(f->done) {
        /* skip to END IF */
        int depth=1;
        while(cur_idx<prog_count&&depth>0) {
            if(!advance_stmt()){cur_idx++;cur_stmt=0;if(cur_idx>=prog_count)break;}
            const char *p=get_stmt(prog[cur_idx].text,cur_stmt);
            if(!p)continue;
            while(*p==' ')p++;
            if(bstrncmp(p,"IF",2)==0) depth++;
            else if(bstrncmp(p,"END IF",6)==0) { depth--; if(!depth) break; }
        }
        if_top--;
    } else {
        f->active=1; f->done=1; f->else_seen=1;
    }
}

static void exec_endif(void)
{
    if(if_top>0) if_top--;
    else kprintf("?END IF without IF\n");
}

/* SELECT CASE */

static void exec_select(void)
{
    if(sel_top>=CALL_DEPTH){kprintf("?SELECT stack overflow\n");stopped=true;return;}
    kw("CASE"); skip_sp();
    sel_stack[sel_top].val=parse_expr();
    sel_stack[sel_top].matched=0;
    sel_stack[sel_top].done=0;
    sel_top++;
    /* skip to first CASE */
    while(cur_idx<prog_count) {
        if(!advance_stmt()){cur_idx++;cur_stmt=0;if(cur_idx>=prog_count)break;}
        const char *p=get_stmt(prog[cur_idx].text,cur_stmt);
        if(!p)continue;
        while(*p==' ')p++;
        if(bstrncmp(p,"CASE",4)==0) break;
        if(bstrncmp(p,"END SELECT",10)==0){sel_top--;return;}
    }
}

static void exec_case(void)
{
    if(sel_top<=0){kprintf("?CASE without SELECT\n");stopped=true;return;}
    sel_frame_t *f=&sel_stack[sel_top-1];
    if(f->done) {
        /* skip to END SELECT */
        int depth=1;
        while(cur_idx<prog_count&&depth>0) {
            if(!advance_stmt()){cur_idx++;cur_stmt=0;if(cur_idx>=prog_count)break;}
            const char *p=get_stmt(prog[cur_idx].text,cur_stmt);
            if(!p)continue;
            while(*p==' ')p++;
            if(bstrncmp(p,"SELECT",6)==0) depth++;
            else if(bstrncmp(p,"END SELECT",10)==0) { depth--; if(!depth) break; }
        }
        sel_top--;
        return;
    }
    skip_sp();
    /* CASE ELSE */
    if(kw("ELSE")) { f->matched=1; f->done=1; return; }
    /* CASE IS rel expr */
    bool match=false;
    if(kw("IS")) {
        skip_sp();
        int32_t v=f->val, rhs;
        if(kw_any("<>")){ rhs=parse_expr(); match=(v!=rhs); }
        else if(kw_any("<=")){ rhs=parse_expr(); match=(v<=rhs); }
        else if(kw_any(">=")){ rhs=parse_expr(); match=(v>=rhs); }
        else if(*gp=='<'){ gp++; rhs=parse_expr(); match=(v<rhs); }
        else if(*gp=='>'){ gp++; rhs=parse_expr(); match=(v>rhs); }
        else if(*gp=='='){ gp++; rhs=parse_expr(); match=(v==rhs); }
    } else {
        /* CASE val [, val ...] or CASE a TO b */
        do {
            skip_sp();
            int32_t lo=parse_expr();
            skip_sp();
            if(kw("TO")) {
                int32_t hi=parse_expr();
                if(f->val>=lo&&f->val<=hi) match=true;
            } else {
                if(f->val==lo) match=true;
            }
            skip_sp();
        } while(*gp==',' && gp++);
    }
    if(match) { f->matched=1; f->done=1; }
    else {
        /* skip to next CASE or END SELECT */
        while(cur_idx<prog_count) {
            if(!advance_stmt()){cur_idx++;cur_stmt=0;if(cur_idx>=prog_count)break;}
            const char *p=get_stmt(prog[cur_idx].text,cur_stmt);
            if(!p)continue;
            while(*p==' ')p++;
            if(bstrncmp(p,"CASE",4)==0) break;
            if(bstrncmp(p,"END SELECT",10)==0){sel_top--;return;}
        }
    }
}

static void exec_end_select(void)
{
    if(sel_top>0) sel_top--;
    else kprintf("?END SELECT without SELECT\n");
}

/* ON expr GOTO / GOSUB */

static void exec_on(void)
{
    int32_t v=parse_expr();
    skip_sp();
    bool is_sub=false;
    if(kw("GOSUB")) is_sub=true;
    else kw("GOTO");
    /* collect line numbers */
    int lines[32]; int nc=0;
    do {
        skip_sp();
        lines[nc++]=(int)parse_expr();
        skip_sp();
    } while(*gp==','&&gp++&&nc<32);
    if(v<1||v>nc) return;  /* out of range: no branch */
    int idx=find_line_idx(lines[v-1]);
    if(idx<0){kprintf("?Undefined line %d\n",lines[v-1]);stopped=true;return;}
    if(is_sub) {
        if(call_top>=CALL_DEPTH){kprintf("?GOSUB overflow\n");stopped=true;return;}
        call_stack[call_top].ret_idx=cur_idx;
        call_stack[call_top].ret_stmt=cur_stmt+1;
        call_top++;
    }
    jump_to(idx);
}

/* READ / DATA / RESTORE */

static void exec_read(void)
{
    skip_sp();
    if(*gp<'A'||*gp>'Z'){kprintf("?Syntax\n");return;}
    int var=*gp++-'A';
    bool is_str=(*gp=='$'); if(is_str) gp++;
    if(data_ptr>=data_count){kprintf("?Out of DATA\n");stopped=true;return;}
    if(is_str) bstrncpy(str_vars[var],data_items[data_ptr++],MAX_STR_LEN+1);
    else       int_vars[var]=katoi(data_items[data_ptr++]);
}

static void exec_restore(void) { data_ptr=0; }

/* DIM */

/* DIM arrays use this fixed pool; allocated ranges are not reclaimed. */
static int32_t arr_pool[26*MAX_ARRAY_LEN];
static int     arr_pool_ptr=0;

static void exec_dim(void)
{
    skip_sp();
    do {
        if(*gp<'A'||*gp>'Z'){kprintf("?Syntax\n");return;}
        int var=*gp++-'A';
        if(*gp=='%') gp++;
        skip_sp(); if(*gp!='('){kprintf("?Syntax\n");return;}
        gp++;
        int32_t sz=parse_expr()+1;  /* DIM A(10) → 0..10 = 11 elements */
        skip_sp(); if(*gp==')') gp++;
        if(sz<=0||sz>MAX_ARRAY_LEN){kprintf("?Array too large\n");return;}
        if(arr_pool_ptr+sz>26*MAX_ARRAY_LEN){kprintf("?Out of array space\n");return;}
        arr_vars[var]=arr_pool+arr_pool_ptr;
        arr_size[var]=(int)sz;
        arr_pool_ptr+=(int)sz;
        /* zero it */
        for(int i=0;i<(int)sz;i++) arr_vars[var][i]=0;
        skip_sp();
    } while(*gp==','&&gp++);
}

/* CONST */

static void exec_const(void)
{
    skip_sp();
    if(*gp<'A'||*gp>'Z'){kprintf("?Syntax\n");return;}
    char name[16]; int ni=0;
    while((*gp>='A'&&*gp<='Z')||(*gp>='0'&&*gp<='9')||*gp=='_')
        name[ni++]=*gp++;
    name[ni]='\0';
    skip_sp(); if(*gp=='=') gp++;
    int32_t val=parse_expr();
    if(const_count<MAX_CONSTS){
        bstrncpy(consts[const_count].name,name,16);
        consts[const_count].val=val;
        const_count++;
    }
}

/* SWAP */

static void exec_swap(void)
{
    skip_sp();
    if(*gp<'A'||*gp>'Z'){kprintf("?Syntax\n");return;}
    int v1=*gp++-'A'; bool s1=(*gp=='$');if(s1)gp++;
    skip_sp(); if(*gp==',')gp++;
    skip_sp();
    if(*gp<'A'||*gp>'Z'){kprintf("?Syntax\n");return;}
    int v2=*gp++-'A'; bool s2=(*gp=='$');if(s2)gp++;
    if(s1&&s2){
        char tmp[MAX_STR_LEN+1]; bstrcpy(tmp,str_vars[v1]);
        bstrcpy(str_vars[v1],str_vars[v2]); bstrcpy(str_vars[v2],tmp);
    } else if(!s1&&!s2){
        int32_t tmp=int_vars[v1]; int_vars[v1]=int_vars[v2]; int_vars[v2]=tmp;
    } else { kprintf("?Type mismatch\n"); }
}

/* SLEEP */

static void exec_sleep(void)
{
    skip_sp();
    int32_t secs=parse_expr();
    volatile uint32_t n=(uint32_t)secs*4000000UL;
    while(n--) __asm__ volatile("nop");
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN STATEMENT DISPATCHER
   ═══════════════════════════════════════════════════════════════════════════ */

static void exec_one_stmt(const char *text)
{
    gp=text;
    skip_sp();
    if(!*gp||*gp=='\r') return;

    /* Comments */
    if(*gp=='\'')  return;
    if(kw("REM"))  return;
    if(kw("DATA")) return;    /* handled by collect_data() */

    /* Control flow */
    if(kw("END")) {
        skip_sp();
        if(kw("IF"))     { exec_endif();      return; }
        if(kw("SELECT")) { exec_end_select(); return; }
        stopped=true; return;
    }
    if(kw("STOP"))   { stopped=true; kprintf("\nBreak\n"); return; }
    if(kw("GOTO"))   { exec_goto();   return; }
    if(kw("GOSUB"))  { exec_gosub();  return; }
    if(kw("RETURN")) { exec_return(); return; }
    if(kw("ON"))     { exec_on();     return; }

    /* FOR / NEXT */
    if(kw("FOR"))    { exec_for();    return; }
    if(kw("NEXT"))   { exec_next();   return; }

    /* WHILE / WEND */
    if(kw("WHILE"))  { exec_while();  return; }
    if(kw("WEND"))   { exec_wend();   return; }

    /* DO / LOOP */
    if(kw("DO"))     { exec_do();     return; }
    if(kw("LOOP"))   { exec_loop();   return; }

    /* IF block */
    if(kw("ELSEIF")) { exec_elseif(); return; }
    if(kw("ELSE"))   { exec_else();   return; }

    /* IF statement */
    if(kw("IF")) {
        int32_t cond=parse_expr();
        skip_sp();
        kw("THEN");
        skip_sp();
        /* Peek: if rest of line is empty → block IF */
        if(!*gp||*gp=='\r'||*gp==':') {
            exec_if_block(cond);
        } else {
            exec_if_single(cond);
        }
        return;
    }

    /* SELECT CASE */
    if(kw("SELECT")) { exec_select(); return; }
    if(kw("CASE"))   { exec_case();   return; }

    /* I/O */
    if(kw("PRINT")||*gp=='?') { if(*gp=='?')gp++; exec_print(); return; }
    if(kw("INPUT"))  { exec_input();  return; }

    /* Variables */
    if(kw("LET"))    { exec_let();    return; }
    if(kw("DIM"))    { exec_dim();    return; }
    if(kw("CONST"))  { exec_const();  return; }
    if(kw("SWAP"))   { exec_swap();   return; }
    if(kw("READ"))   { exec_read();   return; }
    if(kw("RESTORE")){ exec_restore();return; }

    /* Screen */
    if(kw("CLS"))    { ansi_cls();    return; }
    if(kw("LOCATE")) {
        skip_sp(); int32_t r=parse_expr();
        skip_sp(); if(*gp==',')gp++;
        skip_sp(); int32_t c=parse_expr();
        ansi_locate((int)r,(int)c); return;
    }
    if(kw("COLOR")) {
        skip_sp(); int32_t fg=parse_expr();
        int32_t bg=-1;
        skip_sp(); if(*gp==','){gp++;bg=parse_expr();}
        ansi_color((int)fg,(int)bg); return;
    }
    if(kw("BEEP"))   { bputc('\a');   return; }
    if(kw("SLEEP"))  { exec_sleep();  return; }

    if(kw("BYE"))    { bye_flag=true; stopped=true; return; }

    /* Implicit LET: var = expr  or  var$ = str */
    if(*gp>='A'&&*gp<='Z') {
        const char *peek=gp+1;
        if(*peek=='$'||*peek=='('||*peek=='%'||*peek==' '||*peek=='=') {
            exec_let(); return;
        }
    }

    kprintf("?Syntax error: %s\n", text);
}

/* ═══════════════════════════════════════════════════════════════════════════
   RUN LOOP
   ═══════════════════════════════════════════════════════════════════════════ */

static void run_program(void)
{
    collect_data();
    /* zero variables */
    for(int i=0;i<26;i++){ int_vars[i]=0; str_vars[i][0]='\0'; }
    call_top=0; for_top=0; while_top=0; if_top=0; sel_top=0;
    stopped=false; running=true;

    jump_to(0);

    while(!stopped&&cur_idx<prog_count) {
        const char *p=get_stmt(prog[cur_idx].text, cur_stmt);
        if(!p) { cur_idx++; cur_stmt=0; continue; }

        int prev_idx=cur_idx, prev_stmt=cur_stmt;
        exec_one_stmt(p);

        /* If exec didn't change position, advance */
        if(!stopped&&cur_idx==prev_idx&&cur_stmt==prev_stmt) {
            if(!advance_stmt()){ cur_idx++; cur_stmt=0; }
        }
    }

    running=false;
    if(!bye_flag) {
        if(stopped) kprintf("\nBreak in %d\n",
                            cur_idx<prog_count?prog[cur_idx].num:0);
        else        kprintf("\nOk\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   DIRECT MODE COMMANDS
   ═══════════════════════════════════════════════════════════════════════════ */

static void cmd_list(const char *arg)
{
    int from=0, to=99999;
    while(*arg==' ')arg++;
    if(*arg>='0'&&*arg<='9') {
        from=katoi(arg);
        while(*arg>='0'&&*arg<='9')arg++;
        if(*arg=='-'){arg++;to=katoi(arg);}
        else to=from;
    }
    for(int i=0;i<prog_count;i++) {
        if(prog[i].num<from||prog[i].num>to) continue;
        kprintf(ANSI_CYAN "%5d " ANSI_RESET "%s\n",
                prog[i].num, prog[i].text);
    }
    kprintf("Ok\n");
}

static void cmd_new(void)
{
    prog_count=0; const_count=0; arr_pool_ptr=0;
    for(int i=0;i<26;i++){int_vars[i]=0;str_vars[i][0]='\0';arr_vars[i]=NULL;arr_size[i]=0;}
    call_top=0;for_top=0;while_top=0;if_top=0;sel_top=0;
    kprintf("Ok\n");
}

static void cmd_renum(const char *arg)
{
    /* RENUM [start[, step]] */
    int start=10, step=10;
    while(*arg==' ')arg++;
    if(*arg>='0'&&*arg<='9'){
        start=katoi(arg);
        while(*arg>='0'&&*arg<='9')arg++;
        while(*arg==' ')arg++;
        if(*arg==','){arg++; while(*arg==' ')arg++; step=katoi(arg);}
    }
    /* Build old→new map */
    int old_nums[MAX_LINES], new_nums[MAX_LINES];
    for(int i=0;i<prog_count;i++){
        old_nums[i]=prog[i].num;
        new_nums[i]=start+i*step;
    }
    /* Renumber */
    for(int i=0;i<prog_count;i++) prog[i].num=new_nums[i];
    /* Patch line references in raw text; this does not parse strings or comments. */
    for(int i=0;i<prog_count;i++) {
        char newtext[MAX_LINE_LEN]; int ni=0;
        const char *p=prog[i].text;
        while(*p&&ni<MAX_LINE_LEN-1) {
            /* look for GOTO/GOSUB/THEN/ELSE followed by line number */
            bool patch=false;
            if(bstrncmp(p,"GOTO",4)==0||(bstrncmp(p,"THEN",4)==0)||(bstrncmp(p,"GOSUB",5)==0)) {
                int klen=(p[2]=='S')?5:4;
                for(int k=0;k<klen;k++) newtext[ni++]=*p++;
                while(*p==' ') newtext[ni++]=*p++;
                if(*p>='0'&&*p<='9') {
                    int oldln=0;
                    while(*p>='0'&&*p<='9'){oldln=oldln*10+(*p++-'0');}
                    /* find new number */
                    int nln=oldln;
                    for(int j=0;j<prog_count;j++) if(old_nums[j]==oldln){nln=new_nums[j];break;}
                    /* write nln */
                    char tmp[12]; int ti=0;
                    if(nln==0){newtext[ni++]='0';}
                    else{while(nln){tmp[ti++]='0'+nln%10;nln/=10;}while(ti--)newtext[ni++]=tmp[ti];}
                    patch=true;
                }
            }
            if(!patch) newtext[ni++]=*p++;
        }
        newtext[ni]='\0';
        bstrncpy(prog[i].text,newtext,MAX_LINE_LEN);
    }
    kprintf("Ok\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   BANNER & REPL
   ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Simple UART line input for BASIC interpreter
 */
static void basic_getline(char *buf, int maxlen)
{
    int pos = 0;

    while (1) {
        char c = uart_getc();

        // Handle backspace
        if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                uart_putc('\b');
                uart_putc(' ');
                uart_putc('\b');
            }
            continue;
        }

        // Handle enter
        if (c == '\r' || c == '\n') {
            buf[pos] = '\0';
            uart_putc('\r');
            uart_putc('\n');
            return;
        }

        // Handle printable characters
        if (c >= 32 && c < 127 && pos < maxlen - 1) {
            buf[pos++] = c;
            uart_putc(c);
        }
    }
}

static void print_banner(void)
{
    ansi_reset();
    bputs("\r\n");
    bputs(ANSI_CYAN);
    bputs(" ┌──────────────────────────────────────────────────────┐\r\n");
    bputs(" │                                                      │\r\n");
    bputs(" │   " ANSI_WHITE "OS/2 Warp BASIC  Version 4.5  ARM64 Bare-Metal" ANSI_CYAN "   │\r\n");
    bputs(" │                                                      │\r\n");
    bputs(" │   " ANSI_YELLOW "60904 Bytes free" ANSI_CYAN "                                   │\r\n");
    bputs(" │                                                      │\r\n");
    bputs(" └──────────────────────────────────────────────────────┘\r\n");
    bputs(ANSI_RESET);
    bputs("\r\n");
    bputs("Ok\r\n");
}

void basic_run(void)
{
    print_banner();

    static char line[MAX_LINE_LEN];

    for(;;) {
        /* Prompt */
        bputs(ANSI_GREEN);

        basic_getline(line, sizeof(line));
        bputs(ANSI_RESET);

        /* Uppercase (QB is case-insensitive for keywords) */
        bool in_str=false;
        for(int i=0;line[i];i++){
            if(line[i]=='"') in_str=!in_str;
            if(!in_str&&line[i]>='a'&&line[i]<='z') line[i]-=32;
        }

        if(!line[0]) continue;

        /* Direct-mode commands */
        if(bstrcmp(line,"BYE")==0||bstrcmp(line,"QUIT")==0||bstrcmp(line,"SYSTEM")==0) break;
        if(bstrcmp(line,"NEW")==0)  { cmd_new();  continue; }
        if(bstrcmp(line,"RUN")==0)  { bye_flag=false; stopped=false; run_program(); if(bye_flag) break; continue; }
        if(bstrncmp(line,"LIST",4)==0)  { cmd_list(line+4);  continue; }
        if(bstrncmp(line,"RENUM",5)==0) { cmd_renum(line+5); continue; }
        if(bstrcmp(line,"CLS")==0)  { ansi_cls(); continue; }

        /* Numbered line → store */
        if(line[0]>='0'&&line[0]<='9') {
            char *p=line;
            int num=0;
            while(*p>='0'&&*p<='9') num=num*10+(*p++-'0');
            while(*p==' ') p++;
            insert_line(num, p);
            continue;
        }

        /* Immediate execution */
        bye_flag=false; stopped=false;
        exec_one_stmt(line);
        if(bye_flag) break;
        if(!stopped) bputs("Ok\r\n");
    }

    ansi_reset();
    bputs("\r\n");
}
