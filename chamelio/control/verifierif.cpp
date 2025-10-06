#include "prevail/crab_verifier.hpp"
#include "asm_syntax.hpp"
#include "asm_unmarshal.hpp"
#include "bpftime-verifier/platform-impl.hpp"
#include "verifierif.h"

static InstructionSeq convert_raw_instr(void *raw_instr, size_t size)
{
  size_t num_instr, i;
  printf("Converting raw eBPF instructions of size %zu bytes\n", size);
  // TBC: if the provided size isn't a multiple of ebpf_inst size throw error. Could be relaxed later on if needed
  if (size % sizeof(ebpf_inst) != 0)
  {
    throw std::runtime_error("Invalid eBPF bytecode size");
  }

  num_instr = size / sizeof(ebpf_inst);
  auto *instrs = reinterpret_cast<ebpf_inst *>(raw_instr);

  raw_program prog;
  prog.filename = "ebpf_program";
  prog.section = ".text";

  for (i = 0; i < num_instr; i++)
  {
    printf("instruction %zu\n", i);
    prog.prog.push_back(instrs[i]);
  }
  std::vector<std::vector<std::string>> notes;
  auto unmarshal_result = unmarshal(prog, notes);
  if (std::holds_alternative<std::string>(unmarshal_result))
  {
    printf("eBPF unmarshal error: %s\n", std::get<std::string>(unmarshal_result).c_str());
    throw std::runtime_error(std::get<std::string>(unmarshal_result));
  }
  return std::get<InstructionSeq>(unmarshal_result);
}

bool verify_ebpf_cham(void *raw_instr, size_t size)
{
  printf("Verifying eBPF program of size %zu bytes\n", size);
  InstructionSeq ins;
  ebpf_verifier_stats_t stats{};
  ebpf_verifier_options_t options{};
  options.check_termination = true;
  options.no_simplify = true;
  options.assume_assertions = false;
  options.print_invariants = false;
  options.print_failures = true;

  // instrs = convert_raw_instr(raw_instr, size);

  size_t num_instr, i;
  printf("Converting raw eBPF instructions of size %zu bytes\n", size);
  // TBC: if the provided size isn't a multiple of ebpf_inst size throw error. Could be relaxed later on if needed
  if (size % sizeof(ebpf_inst) != 0)
  {
    throw std::runtime_error("Invalid eBPF bytecode size");
  }

  num_instr = size / sizeof(ebpf_inst);
  auto *instrs = reinterpret_cast<ebpf_inst *>(raw_instr);

  raw_program prog;
  prog.filename = "ebpf_program";
  prog.section = "xdp";

  for (i = 0; i < num_instr; i++)
  {
    printf("instruction %zu\n", i);
    prog.prog.push_back(instrs[i]);
  }
  std::vector<std::vector<std::string>> notes;
  auto unmarshal_result = unmarshal(prog, notes);
  if (std::holds_alternative<std::string>(unmarshal_result))
  {
    printf("eBPF unmarshal error: %s\n", std::get<std::string>(unmarshal_result).c_str());
    throw std::runtime_error(std::get<std::string>(unmarshal_result));
  }
  ins = std::get<InstructionSeq>(unmarshal_result);

  // program_info info;
  prog.info.platform = &bpftime::bpftime_platform_spec;

  // TODO: define a function for map descriptors later on
  prog.info.map_descriptors = {};

  static const ebpf_context_descriptor_t ctx_descr = {
      .size = 0,
      .data = -1,
      .end = -1,
      .meta = -1,
  };

  // Using this as a generic type for now which should hopefully pass
  prog.info.type = EbpfProgramType{
      .name = "xdp",
      .context_descriptor = &ctx_descr,
      .platform_specific_data = 0,
      .section_prefixes = {"xdp"},
      .is_privileged = false,
  };

  stats.max_instruction_count = stats.total_unreachable =
      stats.total_warnings = 0;

  std::ostringstream oss;
  printf("Starting eBPF verification\n");
  bool res = ebpf_verify_program(oss, ins, prog.info, nullptr, &stats);
  printf("Finished eBPF verification\n");
  if (!res)
  {
    printf("eBPF verification failed\n");
    std::cerr << oss.str();
  }
  printf("eBPF verification succeeded\n");
  return res;
}