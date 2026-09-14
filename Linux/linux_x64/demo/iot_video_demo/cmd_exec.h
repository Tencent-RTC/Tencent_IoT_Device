#ifndef _CMD_EXEC_H_
#define _CMD_EXEC_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*cmd_callback_t)(int argc, char **argv);
int cmd_exec_register(const char *cmd_name, cmd_callback_t callback, const char *help);
int cmd_exec_init(void);
void cmd_exec_exit(void);
int cmd_exec_process(void);

#ifdef __cplusplus
}
#endif

#endif
