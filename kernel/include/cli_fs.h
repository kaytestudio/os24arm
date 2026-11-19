#ifndef CLI_FS_H
#define CLI_FS_H

#include "types.h"

/* Filesystem / Directory commands */
void cli_cmd_dir   (const char *arg);
void cli_cmd_cd    (const char *arg);
void cli_cmd_md    (const char *arg);   /* alias: mkdir */
void cli_cmd_rd    (const char *arg);   /* alias: rmdir */
void cli_cmd_type  (const char *arg);
void cli_cmd_copy  (const char *arg);
void cli_cmd_xcopy (const char *arg);
void cli_cmd_move  (const char *arg);
void cli_cmd_del   (const char *arg);   /* alias: erase */
void cli_cmd_ren   (const char *arg);   /* alias: rename */
void cli_cmd_attrib(const char *arg);
void cli_cmd_tree  (const char *arg);

/* Volume / device commands */
void cli_cmd_vol    (const char *arg);
void cli_cmd_chkdsk (const char *arg);
void cli_cmd_format (const char *arg);
void cli_cmd_mount  (const char *arg);
void cli_cmd_umount (const char *arg);

/* System commands */
void cli_cmd_ver   (void);
void cli_cmd_echo  (const char *arg);
void cli_cmd_set   (const char *arg);
void cli_cmd_path  (const char *arg);
void cli_cmd_prompt(const char *arg);
void cli_cmd_date  (void);
void cli_cmd_time  (void);
void cli_cmd_mem   (void);
void cli_cmd_pause (void);
void cli_cmd_cls   (void);

/* State accessors */
const char *cli_fs_get_cwd(void);
const char *cli_fs_get_prompt(void);
bool        cli_fs_echo_on(void);

#endif /* CLI_FS_H */
