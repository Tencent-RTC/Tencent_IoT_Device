#include "cmd_exec.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PIPE_PATH "/tmp/tciot"
#define MAX_CMD_LENGTH 256
#define MAX_ARGS 10
#define MAX_COMMANDS 16

typedef struct {
  const char *cmd_name;
  cmd_callback_t callback;
  const char *help;
} cmd_entry_t;

static int pipe_fd = -1;
static cmd_entry_t cmd_table[MAX_COMMANDS];
static int cmd_count = 0;

// ==================== 内部函数前向声明 ====================

static int help_callback(int argc, char **argv);
static int parse_command(char *cmd_line, char **argv, int max_args);

// ==================== 公开接口 ====================

int cmd_exec_init(void) {
  struct stat st;

  // 检查管道是否存在，如果存在则删除
  if (stat(PIPE_PATH, &st) == 0) {
    if (S_ISFIFO(st.st_mode)) {
      printf("Removing existing pipe: %s\n", PIPE_PATH);
      if (unlink(PIPE_PATH) != 0) {
        perror("Failed to remove existing pipe");
        return -1;
      }
    }
  }

  // 创建命名管道
  if (mkfifo(PIPE_PATH, 0666) != 0) {
    perror("Failed to create pipe");
    return -1;
  }
  printf("pipe created: %s\n", PIPE_PATH);

  cmd_count = 0;
  cmd_exec_register("help", help_callback, "Show help");

  return 0;
}

void cmd_exec_exit(void) {
  if (pipe_fd != -1) {
    close(pipe_fd);
    pipe_fd = -1;
  }

  if (unlink(PIPE_PATH) != 0) {
    perror("Failed to remove pipe");
  }

  cmd_count = 0;
}

// 注册命令回调函数
int cmd_exec_register(const char *cmd_name, cmd_callback_t callback, const char *help) {
  if (cmd_name == NULL || callback == NULL) {
    return -1;
  }

  if (cmd_count >= MAX_COMMANDS) {
    printf("Cannot register command '%s': max num %d\n", cmd_name, MAX_COMMANDS);
    return -1;
  }

  cmd_table[cmd_count].cmd_name = cmd_name;
  cmd_table[cmd_count].callback = callback;
  cmd_table[cmd_count].help = help ? help : "";
  cmd_count++;

  return 0;
}

// 处理命令的主函数（非阻塞）
int cmd_exec_process(void) {
  static int first_run = 1;
  char buffer[MAX_CMD_LENGTH];

  // 第一次运行时打开管道（非阻塞模式）
  if (first_run) {
    pipe_fd = open(PIPE_PATH, O_RDONLY | O_NONBLOCK);
    if (pipe_fd == -1) {
      if (errno == ENXIO) {
        // 没有写入者，正常情况
        return 0;
      }
      perror("Failed to open pipe");
      return -1;
    }
    first_run = 0;
  }

  // 尝试读取命令
  ssize_t bytes_read = read(pipe_fd, buffer, sizeof(buffer) - 1);

  if (bytes_read > 0) {
    buffer[bytes_read] = '\0';
    // printf("Received command: %s\n", buffer);

    // 去除换行符
    char *newline = strchr(buffer, '\n');
    if (newline) {
      *newline = '\0';
    }

    char *argv[MAX_ARGS];
    int argc = parse_command(buffer, argv, MAX_ARGS);

    if (argc > 0) {
      for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].cmd_name, argv[0]) == 0) {
          printf("Executing command: %s\n", argv[0]);
          cmd_table[i].callback(argc, argv);
          return 0;
        }
      }
      printf("Unknown command: %s\n", argv[0]);
      help_callback(0, NULL);
    }
  } else if (bytes_read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    perror("Error reading from pipe");
    return -1;
  }

  return 0;
}

// ==================== 内部实现 ====================

static int help_callback(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("Usage: echo \"command\" > " PIPE_PATH "\n");
  printf("commands list:\n");
  for (int i = 0; i < cmd_count; i++) {
    printf("  %s: %s\n", cmd_table[i].cmd_name, cmd_table[i].help);
  }
  return 0;
}

static int parse_command(char *cmd_line, char **argv, int max_args) {
  int argc = 0;
  char *token = strtok(cmd_line, " \t\n\r");

  while (token != NULL && argc < max_args) {
    argv[argc++] = token;
    token = strtok(NULL, " \t\n\r");
  }

  return argc;
}
