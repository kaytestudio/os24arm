/* userland/tests/rexx/ext.cmd - external routines: extsub.cmd (found in
 * this program's directory) as a function and as a subroutine, its own
 * variables, its arguments, PARSE SOURCE's call type, EXIT's value as the
 * result, no result. Its output is compared with ext.out. */
mine = 'caller'
say 'function:' extsub('one', 'two words')
call extsub 'sub'
say 'result:' result
say 'mine still:' mine
say 'by exit:' extsub('exit')
call extsub 'none'
say 'no result:' symbol('RESULT')
say 'nested:' extsub('nested', 5)
