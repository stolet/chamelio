#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Support/raw_ostream.h>

#include "clang.h"
#include "log.h"

namespace
{

/* Thread-local output sink used by the action factory boilerplate. */
thread_local std::vector<uint8_t> *g_bc_out = nullptr;

/* Frontend action that captures the module and serializes it to bitcode; this
 * is the only hook ClangTool gives us to intercept the generated LLVM IR.
 */
class ClangEmitAction : public clang::EmitLLVMOnlyAction
{
protected:
  void EndSourceFileAction() override
  {
    clang::EmitLLVMOnlyAction::EndSourceFileAction();
    auto module = takeModule();
    if (!module || g_bc_out == nullptr)
      return;
    llvm::SmallVector<char, 0> buffer;
    llvm::raw_svector_ostream os(buffer);
    llvm::WriteBitcodeToFile(*module, os);
    g_bc_out->assign(buffer.begin(), buffer.end());
  }
};

/* Strip output/dependency flags and force LLVM bitcode generation.
 * We reuse compile commands from the build system, but any file output or
 * dependency-tracking flags are meaningless for in-memory bitcode and can
 * cause unwanted file writes, so we remove them here.
 */
clang::tooling::ArgumentsAdjuster make_clang_adjuster()
{
  return [](const clang::tooling::CommandLineArguments &args,
            llvm::StringRef)
  {
    clang::tooling::CommandLineArguments out;
    for (size_t i = 0; i < args.size(); ++i)
    {
      /* Drop flags that expect a following filename/target; 
         we never write these files. */
      if (args[i] == "-o" || args[i] == "-MF" ||
          args[i] == "-MT" || args[i] == "-MQ")
      {
        ++i;
        continue;
      }
      /* Drop dependency generation toggles to avoid writing .d files. */
      if (args[i] == "-MD" || args[i] == "-MMD" || args[i] == "-MP")
        continue;
      /* Remove syntax-only mode so the frontend actually emits LLVM IR. */
      if (args[i] == "-fsyntax-only")
        continue;
      out.push_back(args[i]);
    }
    out.push_back("-emit-llvm");
    out.push_back("-O3");
    return out;
  };
}

}

extern "C" int clang_compile(const char *build_dir,
    const char *src_path, void **out_data, size_t *out_len)
{
  std::string error;
  std::vector<uint8_t> bc;

  if (!build_dir || !src_path || !out_data || !out_len)
  {
    LOG_ERROR("failed to parse parameters");
    return -1;
  }

  auto cdb = clang::tooling::CompilationDatabase::loadFromDirectory(
      build_dir, error);
  if (!cdb)
  {
    LOG_ERROR("failed to load compilation database: %s", error.c_str());
    return -1;
  }

  if (cdb->getCompileCommands(src_path).empty())
  {
    LOG_ERROR("no compile commands for clang source: %s", src_path);
    return -1;
  }

  clang::tooling::ClangTool tool(*cdb, {src_path});
  tool.clearArgumentsAdjusters();
  tool.appendArgumentsAdjuster(make_clang_adjuster());

  /* ClangTool needs a FrontendAction factory with no args, so
     we use a thread-local sink to pass the output buffer into the action. */
  g_bc_out = &bc;
  auto factory = clang::tooling::newFrontendActionFactory<ClangEmitAction>();
  int run_rc = tool.run(factory.get());
  g_bc_out = nullptr;
  if (run_rc != 0 || bc.empty())
  {
    LOG_ERROR("failed to compile clang bytecode (rc=%d, src=%s)",
        run_rc, src_path);
    return -1;
  }

  *out_data = malloc(bc.size());
  if (*out_data == nullptr)
  {
    LOG_ERROR("failed to allocate clang bytecode buffer");
    return -1;
  }

  memcpy(*out_data, bc.data(), bc.size());
  *out_len = bc.size();
  return 0;
}
