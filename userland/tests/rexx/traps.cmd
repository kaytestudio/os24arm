/* userland/tests/rexx/traps.cmd - conditions: SIGNAL ON SYNTAX (RC, SIGL,
 * ERRORTEXT, SOURCELINE, CONDITION), a trap off once it has fired, SYNTAX
 * inside a routine (handled there), SIGNAL ON NOVALUE, CALL ON ERROR (DELAY
 * during the handler, on again after), SIGNAL ON ERROR NAME, SIGNAL inside a
 * routine. Its output (without stderr: the shells' messages) is compared
 * with traps.out. */
signal on syntax
x = 1 / 0
say 'not reached'
Syntax:
say 'syntax:' rc errortext(rc) 'at line' sigl
say 'source:' strip(sourceline(sigl))
say 'condition:' condition('C') condition('I') condition('S')
say 'lines:' (sourceline() > 40)
signal on syntax name InRoutine
say 'routine:' f(5)
signal off syntax
signal on novalue
say 'value:' undefined_thing
say 'not reached'
NoValue:
say 'novalue:' condition('D') 'at line' sigl
signal off novalue
say 'off again:' plain_symbol
calls = 0
call on error name Failed
'nosuch_cmd_x1'
say 'after the call: count' calls
'nosuch_cmd_x2'
say 'again: count' calls condition('S')
call off error
signal on error name Jumped
'nosuch_cmd_x3'
say 'not reached'
Jumped:
say 'jumped:' condition('C') condition('I') condition('D') (rc \= 0)
say 'signal in a routine:' g()
exit 0

f: procedure
  arg n
  y = n / 0
  return 'no'
InRoutine:
  return 'recovered' rc 'from line' sigl

Failed:
  calls = value('calls') + 0
  if datatype(calls, 'W') = 0 then calls = 0
  calls = calls + 1
  say 'handler:' condition('C') condition('I') condition('S') condition('D') (rc \= 0)
  return

g: procedure
  signal Skip
  return 'no'
Skip:
  return 'skipped'
