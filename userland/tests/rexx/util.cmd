/* userland/tests/rexx/util.cmd - RexxUtil, as OS/2 scripts start: RxFuncAdd
 * and SysLoadFuncs, then SysMkDir, SysFileTree (files, directories,
 * recursive, names only), SysFileSearch, SysFileDelete, SysRmDir,
 * SysTempFileName, SysOS2Ver. Its output is compared with util.out. */
call RxFuncAdd 'SysLoadFuncs', 'RexxUtil', 'SysLoadFuncs'
call SysLoadFuncs
say 'registered:' RxFuncQuery('SysFileTree') 'version' SysOS2Ver()
d = 'rxutil.tmp'
call SysRmDir d'\sub'; call SysFileDelete d'\a.txt'; call SysFileDelete d'\b.dat'; call SysFileDelete d'\sub\c.txt'; call SysRmDir d
say 'mkdir:' SysMkDir(d) SysMkDir(d'\sub') SysMkDir(d)
call lineout d'\a.txt', 'alpha line'
call lineout d'\a.txt', 'Beta line'
call lineout d'\a.txt'
call lineout d'\b.dat', 'data'
call lineout d'\b.dat'
call lineout d'\sub\c.txt', 'deep alpha'
call lineout d'\sub\c.txt'
call SysFileTree d'\*.txt', 'f.', 'FO'
say 'txt files:' f.0 filespec('name', f.1)
call SysFileTree d'\*', 'all.', 'BO'
say 'files and dirs:' all.0
call SysFileTree d'\*', 'dirs.', 'DO'
say 'dirs:' dirs.0 filespec('name', dirs.1)
call SysFileTree d'\*.txt', 'deep.', 'FSO'
say 'recursive:' deep.0
call SysFileTree d'\b.dat', 'long.', 'F'
say 'size, attributes:' word(long.1, 3) word(long.1, 4)
call SysFileSearch 'alpha', d'\a.txt', 'hit.'
say 'search:' hit.0 hit.1
call SysFileSearch 'ALPHA', d'\a.txt', 'cs.', 'C'
say 'case:' cs.0
call SysFileSearch 'line', d'\a.txt', 'num.', 'N'
say 'numbered:' num.2
t = SysTempFileName(d'\tmp???.x')
say 'temp:' length(filespec('name', t)) (stream(t, 'C', 'QUERY EXISTS') = '')
say 'delete:' SysFileDelete(d'\a.txt') SysFileDelete(d'\b.dat') SysFileDelete(d'\nosuch') SysFileDelete(d'\sub\c.txt')
say 'rmdir:' SysRmDir(d'\sub') SysRmDir(d)
