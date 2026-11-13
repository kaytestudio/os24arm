/**
 * @file dos.h
 * @brief DOS-style command implementations for OS/2 Warp ARM64
 *
 * Each command takes the same (argc, argv) shape as the kernel shell's
 * other cmd_* handlers and talks to the system exclusively through the
 * os2api DOS-call bridge (DosOpen/DosFindFirst/DosCreateDir/...), never
 * touching vfs_* directly - mirroring how a real DOS/OS2 command-line
 * utility only ever calls DOSCALLS.DLL.
 */

#ifndef DOS_H
#define DOS_H

void dos_dir(int argc, char **argv);
void dos_echo(int argc, char **argv);
void dos_cls(int argc, char **argv);
void dos_exit(int argc, char **argv);
void dos_type(int argc, char **argv);
void dos_copy(int argc, char **argv);
void dos_move(int argc, char **argv);
void dos_tree(int argc, char **argv);
void dos_find(int argc, char **argv);
void dos_more(int argc, char **argv);
void dos_date(int argc, char **argv);
void dos_time(int argc, char **argv);
void dos_del(int argc, char **argv);
void dos_ren(int argc, char **argv);
void dos_md(int argc, char **argv);
void dos_rd(int argc, char **argv);
void dos_cd(int argc, char **argv);
void dos_vol(int argc, char **argv);
void dos_attrib(int argc, char **argv);
void dos_chkdsk(int argc, char **argv);
void dos_format(int argc, char **argv);
void dos_doskey(int argc, char **argv);
void dos_comp(int argc, char **argv);
void dos_fc(int argc, char **argv);
void dos_debug(int argc, char **argv);

/**
 * Full CMD.EXE-style dispatcher for the command set above (with the usual
 * DOS aliases: ERASE/DEL, RENAME/REN, MKDIR/MD, RMDIR/RD, CHDIR/CD).
 * Returns 1 and executes the command if argv[0] names one of the commands
 * above, 0 if it doesn't recognize argv[0] (caller should try its own
 * built-ins or report "Bad command"). Uppercases argv[0] in place.
 *
 * Shared by the in-kernel shell (kernel/src/main.c) and the standalone
 * CMD.EXE build (dos/cmd_main.c) so both use identical command handling.
 */
int dos_shell_dispatch(int argc, char **argv);

/**
 * Shared command-line reader for both shells (see dos_commands.c's big
 * comment above dos_doskey() for why this, not dos_doskey() itself, is
 * where DOSKEY's history/macro/line-editing actually lives): reads one
 * line from the UART into buffer (NUL-terminated, returns its length).
 *
 * Before DOSKEY has been run once, this is a plain byte-at-a-time reader
 * (backspace, Ctrl+C, printable chars) - identical to what both shells
 * used to have inline. Once installed, it adds history (arrow keys, F7
 * list, F8 prefix search, F9 select-by-number), in-line editing
 * (Left/Right/Home/End/Delete/Insert, Esc clears the line), and macro
 * expansion. A macro using $T (multiple commands) comes back as multiple
 * '\n'-separated segments in buffer - the caller must split on '\n' and
 * dispatch each piece as its own command line.
 *
 * idle is called (spin-polled, non-blocking on the UART) whenever no
 * input is waiting - kernel_shell() uses it to keep servicing the GUI's
 * workplace-hotkey/pointer polling while waiting for a keypress; pass
 * NULL (as CMD.EXE does) if there's nothing else to do meanwhile.
 */
typedef void (*dos_idle_fn)(void);

/* A VFS path the way the user sees it, with its drive letter:
 * "/h/fonts" -> "D:\fonts" when the volume mounted at \h is D:. */
void dos_display_path(const char *vfs_path, char *out, int out_size);
/* The command prompt for the current directory: "[C:\h]> ". */
void dos_prompt(char *out, int out_size);
int dos_read_line(const char *prompt, char *buffer, int max_len, dos_idle_fn idle);

#endif /* DOS_H */
