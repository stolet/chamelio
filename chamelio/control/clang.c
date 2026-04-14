#include <clang-c/Index.h>
#include <clang-c/CXCompilationDatabase.h>
#include <llvm-c/Core.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/TargetMachine.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "clang.h"
#include "log.h"

static void free_args(char **args, unsigned count);
static int push_arg(char **args, unsigned *arg_idx, unsigned max_args,
  const char *arg);
static int run_clang(char *const *args, const char *work_dir);
static int load_compile_commands(const char *build_dir, const char *src_path,
  CXCompilationDatabase *out_db, CXCompileCommands *out_cmds,
  CXCompileCommand *out_cmd);
static int get_command_paths(CXCompileCommand cmd, const char *build_dir,
  char **out_cmd_dir, char **out_cmd_file);
static int build_clang_args(CXCompileCommand cmd, const char *src_path,
  const char *cmd_file, const char *build_dir, const char *const *extra_defs,
  size_t nr_defs, char ***out_args, unsigned *out_argc, char *out_bc_path,
  size_t bc_path_len);
static int read_file_into_memory(const char *path, void **out_data,
  size_t *out_len);


int clang_compile(const char *build_dir, const char *cmd_src_path,
  const char *src_path, const char *const *extra_defs, size_t nr_defs,
  void **out_data, size_t *out_len)
{
  int rc = -1;
  char *cmd_dir = NULL;
  char *cmd_file = NULL;
  char **args = NULL;
  unsigned argc = 0;
  char temp_bc[512];
  CXCompilationDatabase db = NULL;
  CXCompileCommands cmds = NULL;
  CXCompileCommand cmd = NULL;

  if (!build_dir || !cmd_src_path || !src_path || !out_data || !out_len)
  {
    return -1;
  }
  
  *out_data = NULL;
  *out_len = 0;
  temp_bc[0] = '\0';
  
  /* Initialize LLVM */
  LLVMInitializeAllTargets();
  LLVMInitializeAllTargetMCs();
  LLVMInitializeAllAsmPrinters();
  LLVMInitializeAllAsmParsers();

  if (load_compile_commands(build_dir, cmd_src_path, &db, &cmds, &cmd) != 0)
  {
    LOG_ERROR("failed to load compile commands");
    goto cleanup;
  }

  if (get_command_paths(cmd, build_dir, &cmd_dir, &cmd_file) != 0)
  {
    LOG_ERROR("failed to get command paths");
    goto cleanup;
  }

  if (build_clang_args(cmd, src_path, cmd_file, build_dir, extra_defs,
      nr_defs, &args, &argc, temp_bc, sizeof(temp_bc)) != 0)
  {
    LOG_ERROR("failed to build clang args");
    goto cleanup;
  }

  /* clang invocation */
  if (run_clang(args, cmd_dir) != 0)
  {
    LOG_ERROR("failed to compile clang bytecode");
    goto cleanup;
  }

  if (read_file_into_memory(temp_bc, out_data, out_len) != 0)
  {
    LOG_ERROR("failed to read file into memory");
    goto cleanup;
  }

  rc = 0;

cleanup:
  if (temp_bc[0] != '\0')
  {
    remove(temp_bc);
  }
  free_args(args, argc);
  free(cmd_dir);
  free(cmd_file);
  if (cmds)
  {
    clang_CompileCommands_dispose(cmds);
  }
  if (db)
  {
    clang_CompilationDatabase_dispose(db);
  }
  return rc;
}

static int load_compile_commands(const char *build_dir, const char *src_path,
  CXCompilationDatabase *out_db, CXCompileCommands *out_cmds,
  CXCompileCommand *out_cmd)
{
  CXCompilationDatabase_Error db_error;
  CXCompilationDatabase db = NULL;
  CXCompileCommands cmds = NULL;
  unsigned num_cmds;

  if (!build_dir || !src_path || !out_db || !out_cmds || !out_cmd)
  {
    return -1;
  }

  db = clang_CompilationDatabase_fromDirectory(build_dir, &db_error);
  if (db_error != CXCompilationDatabase_NoError || !db)
  {
    LOG_ERROR("failed to load compilation database from %s (error: %d)",
      build_dir, db_error);
    return -1;
  }

  cmds = clang_CompilationDatabase_getCompileCommands(db, src_path);
  num_cmds = cmds ? clang_CompileCommands_getSize(cmds) : 0;

  if (!cmds || num_cmds == 0)
  {
    LOG_ERROR("no compile commands found for %s", src_path);
    clang_CompilationDatabase_dispose(db);
    return -1;
  }

  *out_db = db;
  *out_cmds = cmds;
  *out_cmd = clang_CompileCommands_getCommand(cmds, 0);
  return 0;
}

static int get_command_paths(CXCompileCommand cmd, const char *build_dir,
  char **out_cmd_dir, char **out_cmd_file)
{
  CXString dir_str;
  CXString file_str;
  const char *dir_cstr;
  const char *file_cstr;

  if (!out_cmd_dir || !out_cmd_file)
  {
    return -1;
  }

  dir_str = clang_CompileCommand_getDirectory(cmd);
  dir_cstr = clang_getCString(dir_str);
  if (dir_cstr != NULL)
  {
    *out_cmd_dir = strdup(dir_cstr);
  }
  clang_disposeString(dir_str);

  if (!*out_cmd_dir && build_dir && build_dir[0] != '\0')
  {
    *out_cmd_dir = strdup(build_dir);
  }

  if (*out_cmd_dir == NULL)
  {
    LOG_ERROR("failed to allocate memory for cmd_dir");
    return -1;
  }

  file_str = clang_CompileCommand_getFilename(cmd);
  file_cstr = clang_getCString(file_str);
  if (file_cstr != NULL)
  {
    *out_cmd_file = strdup(file_cstr);
  }
  clang_disposeString(file_str);

  if (file_cstr != NULL && *out_cmd_file == NULL)
  {
    LOG_ERROR("failed to allocate memory for cmd_file");
    return -1;
  }

  return 0;
}

static int build_clang_args(CXCompileCommand cmd, const char *src_path,
  const char *cmd_file, const char *build_dir, const char *const *extra_defs,
  size_t nr_defs, char ***out_args, unsigned *out_argc, char *out_bc_path,
  size_t bc_path_len)
{
  int has_c = 0;
  int has_emit_llvm = 0;
  unsigned num_args;
  size_t max_args;
  unsigned arg_idx = 0;
  char **args = NULL;

  if (!src_path || !build_dir || !out_args || !out_argc || !out_bc_path)
  {
    return -1;
  }

  out_bc_path[0] = '\0';
  num_args = clang_CompileCommand_getNumArgs(cmd);
  max_args = (size_t)num_args + nr_defs + 10;
  args = calloc(max_args + 1, sizeof(char*));
  if (!args)
  {
    LOG_ERROR("failed to allocate memory for args");
    return -1;
  }

  if (push_arg(args, &arg_idx, max_args, "clang") != 0)
  {
    LOG_ERROR("failed to add clang executable");
    free_args(args, arg_idx);
    return -1;
  }

  for (unsigned i = 0; i < num_args; i++)
  {
    CXString arg_str = clang_CompileCommand_getArg(cmd, i);
    const char *arg = clang_getCString(arg_str);

    /* Skip compiler executable name (first arg) */
    if (i == 0)
    {
      clang_disposeString(arg_str);
      continue;
    }

    /* Skip output flags and their arguments */
    if (strcmp(arg, "-o") == 0 || strcmp(arg, "-MF") == 0 ||
        strcmp(arg, "-MT") == 0 || strcmp(arg, "-MQ") == 0)
    {
      clang_disposeString(arg_str);
      i++; /* Skip next argument too */
      if (i < num_args)
      {
        CXString skip_str = clang_CompileCommand_getArg(cmd, i);
        clang_disposeString(skip_str);
      }
      continue;
    }

    /* Skip dependency generation and syntax-only flags */
    if (strcmp(arg, "-MD") == 0 || strcmp(arg, "-MMD") == 0 ||
        strcmp(arg, "-MP") == 0 || strcmp(arg, "-fsyntax-only") == 0)
    {
      clang_disposeString(arg_str);
      continue;
    }

    /* Skip the source file itself */
    if ((cmd_file && strcmp(arg, cmd_file) == 0) ||
        strcmp(arg, src_path) == 0)
    {
      clang_disposeString(arg_str);
      continue;
    }

    if (strcmp(arg, "-c") == 0)
    {
      has_c = 1;
    }
    else if (strcmp(arg, "-emit-llvm") == 0)
    {
      has_emit_llvm = 1;
    }

    /* Allocate and copy the argument string */
    if (push_arg(args, &arg_idx, max_args, arg) != 0)
    {
      LOG_ERROR("failed to allocate memory for args");
      clang_disposeString(arg_str);
      free_args(args, arg_idx);
      return -1;
    }
    clang_disposeString(arg_str);
  }

  /* Add our own flags */
  if (!has_emit_llvm && push_arg(args, &arg_idx, max_args, "-emit-llvm") != 0)
  {
    LOG_ERROR("failed to add -emit-llvm");
    free_args(args, arg_idx);
    return -1;
  }

  if (push_arg(args, &arg_idx, max_args, "-O3") != 0)
  {
    LOG_ERROR("failed to add -O3");
    free_args(args, arg_idx);
    return -1;
  }

  for (size_t i = 0; i < nr_defs; i++)
  {
    if (push_arg(args, &arg_idx, max_args, extra_defs[i]) != 0)
    {
      LOG_ERROR("failed to add extra clang define");
      free_args(args, arg_idx);
      return -1;
    }
  }

  if (!has_c && push_arg(args, &arg_idx, max_args, "-c") != 0)
  {
    LOG_ERROR("failed to add -c");
    free_args(args, arg_idx);
    return -1;
  }

  snprintf(out_bc_path, bc_path_len, "%s/temp_output.bc", build_dir);
  if (push_arg(args, &arg_idx, max_args, "-o") != 0 ||
      push_arg(args, &arg_idx, max_args, out_bc_path) != 0 ||
      push_arg(args, &arg_idx, max_args, src_path) != 0)
  {
    LOG_ERROR("failed to add clang output args");
    free_args(args, arg_idx);
    return -1;
  }

  args[arg_idx] = NULL;
  *out_args = args;
  *out_argc = arg_idx;
  return 0;
}

static int read_file_into_memory(const char *path, void **out_data,
  size_t *out_len)
{
  FILE *fp;
  long file_size;
  void *data;
  size_t bytes_read;

  if (!path || !out_data || !out_len)
    return -1;

  fp = fopen(path, "rb");
  if (!fp)
  {
    LOG_ERROR("failed to open bitcode file: %s", path);
    return -1;
  }

  fseek(fp, 0, SEEK_END);
  file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (file_size <= 0)
  {
    LOG_ERROR("invalid bitcode file size");
    fclose(fp);
    return -1;
  }

  data = malloc(file_size);
  if (!data)
  {
    LOG_ERROR("failed to allocate memory for bitcode");
    fclose(fp);
    return -1;
  }

  bytes_read = fread(data, 1, file_size, fp);
  fclose(fp);
  if (bytes_read != (size_t)file_size)
  {
    LOG_ERROR("failed to read complete bitcode file");
    free(data);
    return -1;
  }

  *out_data = data;
  *out_len = bytes_read;
  return 0;
}

static void free_args(char **args, unsigned count)
{
  if (!args)
    return;

  for (unsigned i = 0; i < count; i++)
    free(args[i]);

  free(args);
}

static int push_arg(char **args, unsigned *arg_idx, unsigned max_args,
  const char *arg)
{
  if (!args || !arg_idx || !arg || *arg_idx >= max_args)
    return -1;

  args[*arg_idx] = strdup(arg);
  if (!args[*arg_idx])
    return -1;

  (*arg_idx)++;
  return 0;
}

static int run_clang(char *const *args, const char *work_dir)
{
  pid_t pid;
  int status;

  if (!args || !args[0])
  {
    LOG_ERROR("invalid clang args");
    return -1;
  }

  pid = fork();
  if (pid < 0)
  {
    LOG_ERROR("failed to fork clang: %s", strerror(errno));
    return -1;
  }

  if (pid == 0)
  {
    if (work_dir && work_dir[0] != '\0' && chdir(work_dir) != 0)
    {
      LOG_ERROR("chdir(%s) failed: %s", work_dir, strerror(errno));
      _exit(127);
    }

    execvp(args[0], args);
    LOG_ERROR("execvp(%s) failed: %s", args[0], strerror(errno));
    _exit(127);
  }

  if (waitpid(pid, &status, 0) < 0)
  {
    LOG_ERROR("failed to wait for clang: %s", strerror(errno));
    return -1;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
  {
    LOG_ERROR("clang exited with status %d",
      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
  }

  return 0;
}
