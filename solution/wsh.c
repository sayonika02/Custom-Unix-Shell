#define _GNU_SOURCE //strdup, strtok_r
#include "wsh.h"
#include "dynamic_array.h"
#include "utils.h"
#include "hash_map.h"

#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>

int rc;
HashMap *alias_hm = NULL;
DynamicArray *history = NULL;
FILE *batch_file = NULL;

void execute_line(char *line);
int handle_builtin(int argc, char **argv);
char* find_executable(const char *cmd);
void free_argv(int argc, char **argv);

/***************************************************
 * Helper Functions
 ***************************************************/
/**
 * @Brief Free any allocated global resources
 */
void wsh_free(void)
{
  if (alias_hm != NULL) { hm_free(alias_hm); alias_hm = NULL; }
  if (history != NULL) { da_free(history); history = NULL; }
}

/**
 * @Brief Cleanly exit the shell after freeing resources
 *
 * @param return_code The exit code to return
 */
void clean_exit(int return_code)
{
  wsh_free();
  exit(return_code);
}

/**
 * @Brief Print a warning message to stderr and set the return code
 *
 * @param msg The warning message format string
 * @param ... Additional arguments for the format string
 */
void wsh_warn(const char *msg, ...)
{
  va_list args;
  va_start(args, msg);
  vfprintf(stderr, msg, args);
  va_end(args);
  rc = EXIT_FAILURE;
}

/**
 * @Brief Main entry point for the shell
 *
 * @param argc Number of arguments
 * @param argv Array of argument strings
 * @return
 */
int main(int argc, char **argv)
{
  alias_hm = hm_create();
  history = da_create(16);
  setenv("PATH", "/bin", 1);
  rc = EXIT_SUCCESS;

  if (argc > 2)
  {
    wsh_warn(INVALID_WSH_USE);
    clean_exit(EXIT_FAILURE);
  }
  
  if (argc == 1) {
    interactive_main();
  } else {
    rc = batch_main(argv[1]);
  }
  wsh_free();
  return rc;
}

/***************************************************
 * Modes of Execution
 ***************************************************/

/**
 * @Brief Interactive mode: print prompt and wait for user input
 * execute the given input and repeat
 */
void interactive_main(void)
{
  char line[MAX_LINE];
  while (1) {
    printf(PROMPT);
    fflush(stdout);

    if (fgets(line, MAX_LINE, stdin) == NULL) {
      if (ferror(stdin)) { perror("fgets"); }
      break; 
    }
    
    line[strcspn(line, "\n")] = 0;

    char* p = line;
    while (*p && isspace(*p)) p++;
    if (*p == '\0') continue;
    
    execute_line(line);
    da_put(history, line);
    
  }
  clean_exit(rc);
}

int batch_main(const char *script_file)
{
  batch_file = fopen(script_file, "r");
  if (!batch_file) {
    perror("fopen");
    return EXIT_FAILURE;
  }

  char line[MAX_LINE];
  while (fgets(line, MAX_LINE, batch_file) != NULL) { 
    line[strcspn(line, "\n")] = 0;

    char* p = line;
    while (*p && isspace(*p)) p++;
    if (*p == '\0') continue;
    
    execute_line(line);
    da_put(history, line);
  }

  fclose(batch_file); 
  return rc;
}

void execute_single_command(char *command_str) {
    if (batch_file != NULL) {
        fclose(batch_file);
        batch_file = NULL;
    }
    rc = EXIT_SUCCESS;
  
    int argc = 0;
    char *argv[MAX_ARGS];
    
    char *first_word = strdup(command_str);
    char *space_ptr = strchr(first_word, ' ');
    if (space_ptr) *space_ptr = '\0';
    
    char* alias_val = hm_get(alias_hm, first_word);
    char* final_cmd = NULL;

    if (alias_val) {
        final_cmd = strdup(alias_val);
        if (space_ptr) {
            final_cmd = append(final_cmd, " ");
            final_cmd = append(final_cmd, space_ptr + 1);
        }
    } else {
        final_cmd = strdup(command_str);
    }
    free(first_word);

    parseline_no_subst(final_cmd, argv, &argc);
    free(final_cmd);
    
    if (argc == 0) {
        exit(EXIT_SUCCESS);
    }
    
    if (handle_builtin(argc, argv)) {
        free_argv(argc, argv);
        exit(rc);
    }
    
    char* exec_path = find_executable(argv[0]);
    if (!exec_path) {
        char* path_env = getenv("PATH");
        if (!path_env || strlen(path_env) == 0) {
            wsh_warn(EMPTY_PATH);
        } else {
            wsh_warn(CMD_NOT_FOUND, argv[0]);
        }
        free_argv(argc, argv);
        exit(EXIT_FAILURE);
    }

    execv(exec_path, argv);
    
    perror("execv");
    free(exec_path);
    free_argv(argc, argv);
    exit(EXIT_FAILURE);
}

void execute_line(char *line) {
    char *pipe_segments[MAX_ARGS];
    int num_segments = 0;
    char *line_copy = strdup(line);
    if (!line_copy) { perror("strdup"); return; }
    char *saveptr;

    char *token = strtok_r(line_copy, "|", &saveptr);
    while (token != NULL && num_segments < MAX_ARGS) {
        pipe_segments[num_segments++] = token;
        token = strtok_r(NULL, "|", &saveptr);
    }
    
    if (num_segments == 0) {
        free(line_copy);
        return;
    }
    for (int i = 0; i < num_segments; i++) {
        char *trimmed = pipe_segments[i];
        while (*trimmed && isspace(*trimmed)) trimmed++;
        if (*trimmed == '\0') {
            wsh_warn(EMPTY_PIPE_SEGMENT);
            free(line_copy);
            return;
        }
    }

    //LOGIC PATH 1 - SINGLE COMMAND
    if (num_segments == 1) {
        int argc;
        char *argv[MAX_ARGS];
        parseline_no_subst(pipe_segments[0], argv, &argc);

        if (argc > 0) {
            if (strcmp(argv[0], "exit") == 0) {
                if (argc > 1) {
                    wsh_warn(INVALID_EXIT_USE);
                } else {
                    free_argv(argc, argv);
                    free(line_copy);
                    clean_exit(rc);
                }
            }
            else if (!handle_builtin(argc, argv)) {
                pid_t pid = fork();
                if (pid == -1) { perror("fork"); } 
                else if (pid == 0) { execute_single_command(pipe_segments[0]); }
                else {
                    int status;
                    waitpid(pid, &status, 0);
                    if (WIFEXITED(status)) { rc = WEXITSTATUS(status); }
                }
            }
        }
        free_argv(argc, argv);
        free(line_copy);
        return;
    }

    //LOGIC PATH 2 - A PIPELINE (2+ COMMANDS)
    int validation_passed = 1;
    for (int i = 0; i < num_segments; i++) {
        int argc;
        char* argv[MAX_ARGS];
        parseline_no_subst(pipe_segments[i], argv, &argc);
        if (argc > 0) {
            const char* builtins[] = {"exit", "cd", "path", "alias", "unalias", "which", "history", NULL};
            int is_builtin = 0;
            for (int j = 0; builtins[j]; j++) {
                if (strcmp(argv[0], builtins[j]) == 0) {
                    is_builtin = 1;
                    break;
                }
            }
            if (!is_builtin) {
                char* exec_path = find_executable(argv[0]);
                if (!exec_path) {
                    wsh_warn(CMD_NOT_FOUND, argv[0]);
                    validation_passed = 0;
                }
                free(exec_path);
            }
        }
        free_argv(argc, argv);
    }

    if (!validation_passed) {
        rc = EXIT_FAILURE;
        free(line_copy);
        return;
    }

    pid_t pids[num_segments];
    int prev_pipe_fd = -1; 
    int pipe_fds[2];

    for (int i = 0; i < num_segments; i++) {
        if (i < num_segments - 1) {
            if (pipe(pipe_fds) == -1) { perror("pipe"); free(line_copy); return; }
        }
        pids[i] = fork();
        if (pids[i] == -1) { perror("fork"); break; }
        if (pids[i] == 0) { // Child
            if (prev_pipe_fd != -1) { dup2(prev_pipe_fd, STDIN_FILENO); close(prev_pipe_fd); }
            if (i < num_segments - 1) { close(pipe_fds[0]); dup2(pipe_fds[1], STDOUT_FILENO); close(pipe_fds[1]); }
            execute_single_command(pipe_segments[i]);
        } else { // Parent
            if (prev_pipe_fd != -1) { close(prev_pipe_fd); }
            if (i < num_segments - 1) { close(pipe_fds[1]); prev_pipe_fd = pipe_fds[0]; }
        }
    }
    
    free(line_copy);

    for (int i = 0; i < num_segments; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (WIFEXITED(status)) {
            rc = WEXITSTATUS(status);
        }
    }
}

int handle_builtin(int argc, char **argv) {
    if (strcmp(argv[0], "cd") == 0) {
        if (argc > 2) { wsh_warn(INVALID_CD_USE); return 1; }
        char *dir;
        if (argc == 1) {
            dir = getenv("HOME");
            if (dir == NULL) { wsh_warn(CD_NO_HOME); return 1; }
        } else {
            dir = argv[1];
        }
        if (chdir(dir) != 0) { perror("cd"); rc = EXIT_FAILURE; } 
        else { rc = EXIT_SUCCESS; }
        return 1;
    }
    else if (strcmp(argv[0], "path") == 0) {
        if (argc > 2) { wsh_warn(INVALID_PATH_USE); return 1; }
        if (argc == 1) {
            char* path = getenv("PATH");
            if (path) printf("%s\n", path);
            fflush(stdout);
        } else {
            setenv("PATH", argv[1], 1);
        }
        rc = EXIT_SUCCESS;
        return 1;
    }
    else if (strcmp(argv[0], "alias") == 0) {
        if (argc == 1) { 
            hm_print_sorted(alias_hm); 
            fflush(stdout);
            rc = EXIT_SUCCESS;
            return 1;
        } 
        else if (argc >= 3 && strcmp(argv[2], "=") == 0) {
            char* value = (argc > 3) ? strdup(argv[3]) : strdup("");
            for (int i = 4; i < argc; i++) {
                value = append(value, " ");
                value = append(value, argv[i]);
                //throw error message even when quotes are not being used for when there's multiple words in the command
                wsh_warn(INVALID_ALIAS_USE);
            }
            hm_put(alias_hm, argv[1], value); 
            free(value);
            rc = EXIT_SUCCESS;
        } else { 
            wsh_warn(INVALID_ALIAS_USE); 
            return 1; 
        }
        return 1;
    }
    else if (strcmp(argv[0], "unalias") == 0) {
        if (argc != 2) { wsh_warn(INVALID_UNALIAS_USE); return 1; }
        hm_delete(alias_hm, argv[1]);
        rc = EXIT_SUCCESS;
        return 1;
    }
    else if (strcmp(argv[0], "which") == 0) {
        if (argc != 2) { wsh_warn(INVALID_WHICH_USE); return 1; }
        char* cmd = argv[1];
        char* alias_val = hm_get(alias_hm, cmd);
        if (alias_val) { 
            printf(WHICH_ALIAS, cmd, alias_val); 
            fflush(stdout);
            rc = EXIT_SUCCESS;
            return 1; 
        }
        const char* builtins[] = {"exit", "cd", "path", "alias", "unalias", "which", "history", NULL};
        for (int i = 0; builtins[i]; i++) {
            if (strcmp(cmd, builtins[i]) == 0) {
                printf(WHICH_BUILTIN, cmd);
                fflush(stdout);
                rc = EXIT_SUCCESS;
                return 1;
            }
        }
        char* exec_path = find_executable(cmd);
        if (exec_path) {
            printf(WHICH_EXTERNAL, cmd, exec_path); 
            free(exec_path);
        } else {
            printf(WHICH_NOT_FOUND, cmd);
        }
        fflush(stdout);
        rc = EXIT_SUCCESS;
        return 1;
    }
    else if (strcmp(argv[0], "history") == 0) {
        if (argc > 2) { wsh_warn(INVALID_HISTORY_USE); return 1; }
        if (argc == 1) { 
            for (size_t i = 0; i < history->size; i++) {
                printf("%s\n", da_get(history, i));
            }
        } else {
            char *endptr;
            long n = strtol(argv[1], &endptr, 10);
            if (*endptr != '\0' || n <= 0 || (size_t)n > history->size) {
                wsh_warn(HISTORY_INVALID_ARG);
                return 1;
            } else {
                printf("%s\n", da_get(history, n - 1));
            }
        }
        fflush(stdout);
        rc = EXIT_SUCCESS;
        return 1;
    }
    if (strcmp(argv[0], "exit") == 0) {
        exit(rc);
    }
    return 0;
}

char* find_executable(const char *cmd) {
    if (!cmd || strlen(cmd) == 0) return NULL;

    if (strchr(cmd, '/') != NULL) {
        if (access(cmd, X_OK) == 0) return strdup(cmd);
        return NULL;
    }
    
    char *path_env = getenv("PATH");
    if (!path_env || strlen(path_env) == 0) {
        return NULL;
    }
    
    char *path_copy = strdup(path_env);
    char *saveptr;
    char *dir = strtok_r(path_copy, ":", &saveptr);
    char full_path[MAX_LINE];
    
    while(dir) {
        if (strlen(dir) > 0) {
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
            if (access(full_path, X_OK) == 0) {
                free(path_copy);
                return strdup(full_path);
            }
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }
    
    free(path_copy);
    return NULL;
}

void free_argv(int argc, char **argv) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        if (argv[i]) { free(argv[i]); argv[i] = NULL; }
    }
}

/**
 * @Brief Parse a command line into arguments without doing
 * any alias substitutions.
 * Handles single quotes to allow spaces within arguments.
 * (This is the provided function, unchanged).
 *
 * @param cmdline The command line to parse
 * @param argv Array to store the parsed arguments (must be preallocated)
 * @param argc Pointer to store the number of parsed arguments
 */
void parseline_no_subst(const char *cmdline, char **argv, int *argc)
{
  if (!cmdline)
  {
    *argc = 0;
    argv[0] = NULL;
    return;
  }
  char *buf = strdup(cmdline);
  if (!buf)
  {
    perror("strdup");
    clean_exit(EXIT_FAILURE);
  }
  /* Replace trailing newline with space */
  const size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == '\n')
    buf[len - 1] = ' ';
  else
  {
    char *new_buf = realloc(buf, len + 2);
    if (!new_buf)
    {
      perror("realloc");
      free(buf);
      clean_exit(EXIT_FAILURE);
    }
    buf = new_buf;
    strcat(buf, " ");
  }

  int count = 0;
  char *p = buf;
  while (*p && *p == ' ')
    p++; /* skip leading spaces */

  while (*p)
  {
    char *token_start = p;
    char *token = NULL;
    if (*p == '\'')
    {
      token_start = ++p;
      token = strchr(p, '\'');
      if (!token)
      {
        /* Handle missing closing quote - Print `Missing closing quote` to stderr */
        wsh_warn(MISSING_CLOSING_QUOTE);
        free(buf);
        for (int i = 0; i < count; i++) free(argv[i]);
        *argc = 0;
        argv[0] = NULL;
        return;
      }
      *token = '\0';
      p = token + 1;
    }
    else
    {
      token = strchr(p, ' ');
      if (!token)
        break;
      *token = '\0';
      p = token + 1;
    }
    if (count < MAX_ARGS - 1) {
        argv[count] = strdup(token_start);
        if (!argv[count]) {
          perror("strdup");
          for (int i = 0; i < count; i++) free(argv[i]);
          free(buf);
          clean_exit(EXIT_FAILURE);
        }
        count++;
    }
    while (*p && (*p == ' '))
      p++;
  }
  argv[count] = NULL;
  *argc = count;
  free(buf);
}