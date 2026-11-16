/* userland/tests/rexx/queue.cmd - the external data queue (PUSH, QUEUE,
 * PULL, PARSE PULL, QUEUED), a command's output through "| RXQUEUE" (and
 * /LIFO), NOTREADY (SIGNAL ON for a file that isn't there, CALL ON at a
 * file's end), CHARIN. Its output is compared with queue.out. */
queue 'second'
queue 'third'
push 'first'
say 'queued:' queued()
pull a
parse pull b
parse pull c
say 'pulled:' a b c queued()
'echo one two | rxqueue'
'echo three | RXQUEUE /LIFO'
say 'from a command:' queued() 'rc' rc
do while queued() > 0
  parse pull line
  say '  ' line
end
signal on notready
x = linein('no_such_file.txt')
say 'not reached'
NotReady:
say 'notready:' condition('C') condition('I') condition('D')
signal off notready
f = 'queue.tmp'
call lineout f, 'abc'
call lineout f, 'def'
call lineout f
call on notready name AtEnd
ended = 0
n = 0
do until ended
  l = linein(f)
  if \ended then n = n + 1
end
say 'lines read:' n
call stream f, 'C', 'CLOSE'
say 'charin:' charin(f, 1, 2) charin(f) c2x(charin(f, , 3))
call stream f, 'C', 'CLOSE'
call SysFileDelete f
exit

AtEnd:
  ended = 1
  say 'at the end:' condition('C') condition('I') condition('S')
  return
