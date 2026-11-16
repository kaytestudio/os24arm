/* userland/tests/rexx/extsub.cmd - the external routine ext.cmd calls */
parse source . how .
parse arg what, more
mine = 'callee'
select
  when what = 'exit' then exit 'exited' how
  when what = 'none' then return
  when what = 'nested' then return 'square' sq(more)
  otherwise nop
end
return how '['what'] ['more']' arg() symbol('CALLERVAR')
sq: procedure
  return arg(1) * arg(1)
