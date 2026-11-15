/* userland/tests/rexx/test.cmd - REXX's instructions and functions; its
 * output is compared with test.out by userland/tests/rexx/run.sh */
say 'Hello' "REXX" || '!'          /* blank and || concatenation */
x = 3; y = 4
say 'x+y=' x + y 'x*y='||x*y 'abut'y        /* || and abuttal */
say 7 / 2 7 % 2 7 // 2 2 ** 10 1/3 (-5 + 2) 1/3 -5 + 2   /* (a minus after a blank: still subtraction) */
say ('abc' = ' abc ') ('abc' == ' abc ') (10 > 9) ('10' > '9') ('b' >> 'a')
say \ 0 (1 & 0) (1 | 0) (1 && 1)
/* IF / THEN / ELSE, nested */
if x > y then say 'x is bigger'
else if x = y then say 'equal'
else say 'y is bigger'
if x < 5 then do; say 'block: x < 5'; say 'still in it'; end
/* DO loops */
s = ''
do i = 1 to 5; s = s i; end
say 'to:' s
s = ''
do i = 10 to 1 by -3; s = s i; end
say 'by:' s
s = ''
do 3; s = s || '*'; end
say 'count:' s
n = 0
do while n < 4; n = n + 1; end
say 'while:' n
n = 0
do until n >= 3; n = n + 1; end
say 'until:' n
s = ''
do i = 1 to 10
    if i // 2 = 0 then iterate
    if i > 7 then leave
    s = s i
end
say 'iterate/leave:' s
s = ''
do i = 1 for 3; s = s i; end
say 'for:' s
/* SELECT */
do v = 1 to 3
    select
        when v = 1 then say 'select one'
        when v = 2 then do; say 'select two'; end
        otherwise say 'select other'
    end
end
/* stems */
a.1 = 'first'; a.2 = 'second'; a.0 = 2
do i = 1 to a.0; say 'stem' i a.i; end
b. = 'default'
say 'b.7:' b.7
k = 'Q'; c.k = 'by name'; say 'c.q:' c.q 'c.x:' c.x   /* (x is 3: C.3, unset) */
/* functions: internal, recursive, PROCEDURE EXPOSE */
say 'fact(10):' fact(10)
total = 100
call addto 5
say 'total:' total 'result:' result
say 'squares:' squares(4)
/* PARSE */
parse value 'John Smith 42 Lisbon' with first last age rest
say first '|' last '|' age '|' rest
parse value 'key=value;other' with k '=' v ';' o
say k v o
parse value 'abcdefghij' with 1 p1 4 p2 7 p3
say p1 p2 p3
parse upper value 'mixed Case' with w1 w2
say w1 w2
line = 'one two three'
parse var line h t
say h '/' t
/* built-ins */
say length('hello') substr('hello', 2, 3) left('ab', 4, '.') || '|' right('ab', 4) || '|' center('x', 5, '-')
say pos('lo', 'hello') lastpos('l', 'hello') word('a b c', 2) words('a  b c') wordpos('b c', 'a b c')
say strip('  pad  ') || '|' strip('xxyxx', 'B', 'x') space('a   b  c') copies('ab', 3) reverse('abc')
say translate('hello') translate('abc', 'xyz', 'abc') upper('Mixed')
say abs(-3) max(1, 9, 4) min(5, 2) trunc(3.789, 2) sign(-2) format(3.14159, 3, 2)
say c2x('AB') x2c('4142') d2x(255) x2d('FF') c2d('A') d2c(66) ('0F'x == x2c('0f'))
say datatype(123) datatype('abc') datatype('12', 'W') datatype('ab', 'U')
say subword('a b c d', 2, 2) delword('a b c d', 2, 1) insert('XY', 'abcd', 2) overlay('XY', 'abcd', 2) delstr('abcdef', 2, 3)
say verify('123a5', '0123456789') compare('abc', 'abd') wordindex('aa bb', 2) wordlength('aa bbb', 2)
/* SIGNAL, INTERPRET, VALUE, SYMBOL */
interpret 'zz = 6 * 7'
say 'interpret:' zz value('zz') symbol('zz') symbol('nope')
signal skip
say 'not reached'
skip:
say 'signalled'
/* files */
f = 'rexx-test.tmp'
call lineout f, 'line one'
call lineout f, 'line two'
call lineout f
n = 0
do while lines(f) > 0; l = linein(f); n = n + 1; say 'read:' l; end
call stream f, 'C', 'CLOSE'
'rm -f rexx-test.tmp'
say 'rc' rc
exit 3

fact: procedure
    parse arg n
    if n <= 1 then return 1
    return n * fact(n - 1)
addto: procedure expose total
    arg amount
    total = total + amount
    return total
squares: procedure
    arg n
    s = ''
    do i = 1 to n; s = s i*i; end
    return strip(s)
