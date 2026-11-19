/**
 * @file ai.h
 * @brief The AI command: command usage and tips
 *
 * Counts which built-in shell commands are used during the session, shows
 * the counts, and suggests commands that haven't been used yet. Nothing is
 * saved; the counts start over at each boot.
 */

#ifndef AI_H
#define AI_H

/**
 * @brief Count a command typed at the shell (for AI STATS/TIP).
 *
 * Called for every command line; names it doesn't know are ignored.
 *
 * @param[in] name  Command name (argv[0]), already upper-cased.
 */
void ai_record_command(const char *name);

/**
 * @brief Handle the AI command (AI, AI STATUS, AI STATS, AI TIP, AI HELP).
 */
void ai_run(int argc, char **argv);

#endif /* AI_H */
