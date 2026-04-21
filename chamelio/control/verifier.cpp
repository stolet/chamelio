#include <iostream>
#include <string>
#include <vector>
#include <linux/bpf.h>

#include <prevail.hpp>

#include "verifier.h"
#include "cham_fast.h"
#include "log.h"

using namespace prevail;
using std::string;
using std::vector;

extern "C"
{
    static ebpf_context_descriptor_t make_cham_ctx_desc();
    static EbpfProgramType make_cham_program_type(const string &name, 
        ebpf_context_descriptor_t *ctx_descr);
    static EbpfHelperPrototype get_helper_prototype(int32_t id);
    static bool is_helper_usable(int32_t id);
    static void print_analysis_result(Program prog, AnalysisResult res, 
        verbosity_options_t verbosity);

    static void print_analysis_result(Program prog, AnalysisResult res, 
        verbosity_options_t verbosity)
    {
    if (verbosity.print_invariants) 
        print_invariants(std::cout, prog, verbosity.simplify, res);

    if (verbosity.print_failures) 
    {
        if (auto err = res.find_first_error()) 
            print_error(std::cout, *err);

        print_unreachable(std::cout, prog, res);
    }
    }

    static EbpfHelperPrototype get_helper_prototype(int32_t id)
    {
        switch (id) {
        case 1001:
            return EbpfHelperPrototype{
                .name = "ebpf_queue_tail",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };

        case 1002:
            return EbpfHelperPrototype{
                .name = "queue_enqueue",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };

        case 1003:
            return EbpfHelperPrototype{
                .name = "ebpf_memcpy",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };

        case 1004:
            return EbpfHelperPrototype{
                .name = "ebpf_print",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };

        case 1005:
            return EbpfHelperPrototype{
                .name = "ebpf_ipv4_checksum",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };

        case 1006:
            return EbpfHelperPrototype{
                .name = "ebpf_ipv4_udptcp_cksum",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };

        case 1007:
            return EbpfHelperPrototype{
                .name = "ebpf_sched_head",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };

        case 1008:
            return EbpfHelperPrototype{
                .name = "sched_pop",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            }; 

        case 1009:
            return EbpfHelperPrototype{
                .name = "sched_add",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        case 1010:
            return EbpfHelperPrototype{
                .name = "ebpf_map_get",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };

        case 1011:
            return EbpfHelperPrototype{
                .name = "ebpf_map_lookup",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        case 1012:
            return EbpfHelperPrototype{
                .name = "ebpf_queue_head",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        case 1013:
            return EbpfHelperPrototype{
                .name = "queue_dequeue",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        case 1014:
            return EbpfHelperPrototype{
                .name = "ebpf_rdtsc",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        case 1015:
            return EbpfHelperPrototype{
                .name = "ebpf_rate_delay_tsc",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        case 1019:
            return EbpfHelperPrototype{
                .name = "ebpf_now_us",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        case 1016:
            return EbpfHelperPrototype{
                .name = "ebpf_spin_lock",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        case 1017:
            return EbpfHelperPrototype{
                .name = "ebpf_spin_unlock",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        case 1018:
            return EbpfHelperPrototype{
                .name = "sched_remove",
                .return_type = EBPF_RETURN_TYPE_INTEGER,
                .argument_type = {
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_ANYTHING,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                    EBPF_ARGUMENT_TYPE_DONTCARE,
                },
                .reallocate_packet = false,
                .context_descriptor = nullptr,
                .unsupported = false,
            };
        default:
            LOG_ERROR("could not find helper id=%d", id);
            EbpfHelperPrototype unknown{};
            unknown.name = "unknown_helper";
            unknown.return_type = EBPF_RETURN_TYPE_UNSUPPORTED;
            unknown.argument_type[0] = EBPF_ARGUMENT_TYPE_DONTCARE;
            unknown.unsupported = true;
            return unknown;
        }
    }

    static bool is_helper_usable(int32_t id)
    {
        switch (id) 
        {
            case 1001:
            case 1002:
            case 1003:
            case 1004:
            case 1005:
            case 1006:
            case 1007:
            case 1008:
            case 1009:
            case 1010:
            case 1011:
            case 1012:
            case 1013:
            case 1014:
            case 1015:
            case 1016:
            case 1017:
            case 1018:
            case 1019:
                return true;
            default:
                return false;
        }
    }

    int verifier_analyze(const void *ebpf_instr, size_t instr_cnt,
            __u64 shm_len, __u32 perf_iso_max_ins, char *name)
    {
        Program prog;
        RawProgram raw_prog;
        AnalysisResult res;
        ebpf_verifier_options_t options;
        const EbpfInst *instr_ptr;
        std::variant<InstructionSeq, std::string> instr_or_err;
        InstructionSeq instr_seq;
        ebpf_context_descriptor_t ctx_descr;
        EbpfMapDescriptor map_descr;
        ebpf_platform_t cham_platform;

        /* Convert instruction array to vector */
        instr_ptr = static_cast<const EbpfInst *>(ebpf_instr);
        std::vector<EbpfInst> instr(instr_ptr, instr_ptr + instr_cnt);

        /* Set verifier options */
        options.mock_map_fds = false;
        options.allow_division_by_zero = true;
        options.strict = false;
        options.big_endian = std::endian::native == std::endian::big;
        options.cfg_opts.check_for_termination = false;
        options.verbosity_opts.simplify = true;
        options.verbosity_opts.print_line_info = true;
        options.verbosity_opts.dump_btf_types_json = true;
        options.verbosity_opts.print_invariants = true;
        options.verbosity_opts.print_failures = true;

        /* Set program info */
        cham_platform = g_ebpf_platform_linux;
        cham_platform.get_helper_prototype = get_helper_prototype;
        cham_platform.is_helper_usable = is_helper_usable;
        raw_prog.info.platform = &cham_platform;
        ctx_descr = make_cham_ctx_desc();
        raw_prog.info.map_descriptors.push_back(map_descr);
        raw_prog.info.type = make_cham_program_type(std::string{name}, &ctx_descr);
        raw_prog.prog = instr;

        /* Unmarshal ebpf instructions into a Prevail instruction sequence */
        instr_or_err = unmarshal(raw_prog, options);
        if (std::get_if<string>(&instr_or_err)) 
        {
            LOG_ERROR("unmarshaling error");
            return -1;
        }
        
        instr_seq = std::get<InstructionSeq>(instr_or_err);

        /* Create program to be analyzed */
        prog = Program::from_sequence(instr_seq, raw_prog.info, options);

        /* Analyze program */
        res = analyze(prog);
        if (res.failed)
        {
            print_analysis_result(prog, res, options.verbosity_opts);
            return -1;
        }

        if (res.max_loop_count >= 0 &&
            (__u32) res.max_loop_count > perf_iso_max_ins)
        {
            LOG_ERROR("program %s exceeds instruction limit max_loop_count=%llu limit=%u",
                name, (unsigned long long) res.max_loop_count, perf_iso_max_ins);
            return -1;
        }
        LOG_DEBUG("max_loop_count=%d", res.max_loop_count);

        return 0;
    }

    static ebpf_context_descriptor_t make_cham_ctx_desc()
    {
        ebpf_context_descriptor_t ctx_descr = {
            .size = sizeof(struct cham_ebpf_ctx),
            .data = offsetof(struct cham_ebpf_ctx, pkt),
            .end = offsetof(struct cham_ebpf_ctx, pkt_end),
            .meta = -1,
            .qe = offsetof(struct cham_ebpf_ctx, qe),
            .qe_end = offsetof(struct cham_ebpf_ctx, shm_end),
        };

        return ctx_descr;
    }

    static EbpfProgramType make_cham_program_type(const string &name, 
        ebpf_context_descriptor_t *ctx_descr)
    {
        EbpfProgramType type = {
            .name = name,
            .context_descriptor = ctx_descr,
            .platform_specific_data = 0,
            .section_prefixes = {},
            .is_privileged = true,
            };

        return type;
    }
}
