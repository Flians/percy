#pragma once

#include <chrono>
#include <memory>
#include <thread>
#include <mutex>
#include "spec.hpp"
#include "fence.hpp"
#include "chain.hpp"
#include "mig.hpp"
#include "dag_generation.hpp"
#include "tt_utils.hpp"
#include "concurrentqueue.h"
#include "partial_dag.hpp"
#include "solvers.hpp"
#include "encoders.hpp"
#include "cnf.hpp"
#if defined(_WIN32)
#include <limits>
#else
#include <numeric_limits>
#endif

/*******************************************************************************
    This module defines the interface to synthesize Boolean chains from
    specifications. The inspiration for this module is taken from Mathias
    Soeken's earlier exact synthesis algorithm, which has been integrated in
    the ABC synthesis package. That implementation is itself based on earlier
    work by Éen[1] and Knuth[2].

    [1] Niklas Éen, "Practical SAT – a tutorial on applied satisfiability
    solving," 2007, slides of invited talk at FMCAD.
    [2] Donald Ervin Knuth, "The Art of Computer Programming, Volume 4,
    Fascicle 6: Satisfiability," 2015
*******************************************************************************/
namespace percy
{
	using std::chrono::high_resolution_clock;
	using std::chrono::duration;
	using std::chrono::time_point;

    const int PD_SIZE_CONST = 1000; // Some "impossibly large" number

    static inline bool is_trivial(const kitty::dynamic_truth_table& tt)
    {
        kitty::dynamic_truth_table tt_check(tt.num_vars());

        if (tt == tt_check || tt == ~tt_check) {
            return true;
        }

        for (auto i = 0; i < tt.num_vars(); i++) {
            kitty::create_nth_var(tt_check, i);
            if (tt == tt_check || tt == ~tt_check) {
                return true;
            }
        }

        return false;
    }

    synth_result 
    std_synthesize(
        spec& spec, 
        chain& chain, 
        solver_wrapper& solver, 
        std_encoder& encoder,
        synth_stats* stats = NULL)
    {
        assert(spec.get_nr_in() >= spec.fanin);
        spec.preprocess();
        encoder.set_dirty(true);

        if (stats) {
            stats->synth_time = 0;
            stats->sat_time = 0;
            stats->unsat_time = 0;
        }

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            chain.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                chain.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        spec.nr_steps = spec.initial_steps;
        while (true) {
            solver.restart();
            if (!encoder.encode(spec)) {
                spec.nr_steps++;
                continue;
            }

            auto begin = std::chrono::steady_clock::now();
            const auto status = solver.solve(spec.conflict_limit);
            auto end = std::chrono::steady_clock::now();
            auto elapsed_time =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    end - begin
                    ).count();

            if (stats) {
                stats->synth_time += elapsed_time;
            }

            if (status == success) {
                encoder.extract_chain(spec, chain);
                if (spec.verbosity > 2) {
                    //    encoder.print_solver_state(spec);
                }
                if (stats) {
                    stats->sat_time += elapsed_time;
                }
                return success;
            } else if (status == failure) {
                if (stats) {
                    stats->unsat_time += elapsed_time;
                }
                spec.nr_steps++;
            } else {
                return timeout;
            }
        }
    }

    synth_result
    std_cegar_synthesize(
        spec& spec, 
        chain& chain, 
        solver_wrapper& solver, 
        std_encoder& encoder,
        synth_stats* stats = NULL)
    {
        assert(spec.get_nr_in() >= spec.fanin);
        spec.preprocess();
        encoder.set_dirty(true);

        if (stats) {
            stats->synth_time = 0;
            stats->sat_time = 0;
            stats->unsat_time = 0;
        }

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            chain.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                chain.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        spec.nr_rand_tt_assigns = 2 * spec.get_nr_in();
        spec.nr_steps = spec.initial_steps;
        while (true) {
            solver.restart();
            if (!encoder.cegar_encode(spec)) {
                spec.nr_steps++;
                continue;
            }
            while (true) {
                auto begin = std::chrono::steady_clock::now();
                auto stat = solver.solve(spec.conflict_limit);
                auto end = std::chrono::steady_clock::now();
                auto elapsed_time =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        end - begin
                        ).count();

                if (stats) {
                    stats->synth_time += elapsed_time;
                }
                if (stat == success) {
                    encoder.extract_chain(spec, chain);
                    auto sim_tts = chain.simulate();
                    auto xor_tt = (sim_tts[0]) ^ (spec[0]);
                    auto first_one = kitty::find_first_one_bit(xor_tt);
                    if (first_one == -1) {
                        if (stats) {
                            stats->sat_time += elapsed_time;
                        }
                        return success;
                    }
                    // Add additional constraint.
                    if (spec.verbosity) {
                        printf("  CEGAR difference at tt index %ld\n",
                            first_one);
                    }
                    if (!encoder.create_tt_clauses(spec, first_one - 1)) {
                        spec.nr_steps++;
                        break;
                    }
                } else if (stat == failure) {
                    if (stats) {
                        stats->unsat_time += elapsed_time;
                    }
                    spec.nr_steps++;
                    break;
                } else {
                    return timeout;
                }
            }
        }
    }

    std::unique_ptr<solver_wrapper>
    get_solver(SolverType type = SLV_BSAT2)
    {
        solver_wrapper * solver = nullptr;
        std::unique_ptr<solver_wrapper> res;

        switch (type) {
        case SLV_BSAT2:
            solver = new bsat_wrapper;
            break;
#ifdef USE_CMS
        case SLV_CMSAT:
            solver = new cmsat_wrapper;
            break;
#endif
#if defined(USE_GLUCOSE) || defined(USE_SYRUP)
        case SLV_GLUCOSE:
            solver = new glucose_wrapper;
            break;
#endif
#ifdef USE_SATOKO
        case SLV_SATOKO:
            solver = new satoko_wrapper;
            break;
#endif
        default:
            fprintf(stderr, "Error: solver type %d not found", type);
            exit(1);
        }

        res.reset(solver);
        return res;
    }

    std::unique_ptr<encoder>
    get_encoder(solver_wrapper& solver, EncoderType enc_type = ENC_KNUTH)
    {
        encoder * enc = nullptr;
        std::unique_ptr<encoder> res;

        switch (enc_type) {
        case ENC_KNUTH:
            enc = new knuth_encoder(solver);
            break;
        case ENC_EPFL:
            enc = new epfl_encoder(solver);
            break;
        case ENC_BERKELEY:
            enc = new berkeley_encoder(solver);
            break;
        case ENC_FENCE:
            enc = new knuth_fence_encoder(solver);
            break;
        case ENC_DAG:
            enc = new knuth_dag_encoder<2>();
            break;
        default:
            fprintf(stderr, "Error: encoder type %d not found\n", enc_type);
            exit(1);
        }

        res.reset(enc);
        return res;
    }

    std::unique_ptr<enumerating_encoder>
    get_enum_encoder(solver_wrapper& solver, EncoderType enc_type = ENC_KNUTH)
    {
        enumerating_encoder * enc = nullptr;
        std::unique_ptr<enumerating_encoder> res;

        switch (enc_type) {
        case ENC_KNUTH:
            enc = new knuth_encoder(solver);
            break;
        case ENC_EPFL:
            enc = new epfl_encoder(solver);
            break;
        case ENC_FENCE:
            enc = new knuth_fence_encoder(solver);
            break;
        default:
            fprintf(stderr, "Error: enumerating encoder of ctype %d not found\n", enc_type);
            exit(1);
        }

        res.reset(enc);
        return res;
    }


    synth_result 
    fence_synthesize(spec& spec, chain& chain, solver_wrapper& solver, fence_encoder& encoder)
    {
        assert(spec.get_nr_in() >= spec.fanin);
        spec.preprocess();
        encoder.set_dirty(true);

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            chain.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                chain.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        // As the topological synthesizer decomposes the synthesis
        // problem, to fairly count the total number of conflicts we
        // should keep track of all conflicts in existence checks.
        fence f;
        po_filter<unbounded_generator> g(
            unbounded_generator(spec.initial_steps),
            spec.get_nr_out(), spec.fanin);
        int old_nnodes = 1;
        auto total_conflicts = 0;
        while (true) {
            g.next_fence(f);
            spec.nr_steps = f.nr_nodes();

            if (spec.nr_steps > old_nnodes) {
                // Reset conflict count, since this is where other
                // synthesizers reset it.
                total_conflicts = 0;
                old_nnodes = spec.nr_steps;
            }

            solver.restart();
            if (!encoder.encode(spec, f)) {
                continue;
            }

            if (spec.verbosity) {
                printf("  next fence:\n");
                print_fence(f);
                printf("\n");
                printf("nr_nodes=%d, nr_levels=%d\n", f.nr_nodes(),
                    f.nr_levels());
                for (int i = 0; i < f.nr_levels(); i++) {
                    printf("f[%d] = %d\n", i, f[i]);
                }
            }
            auto status = solver.solve(spec.conflict_limit);
            if (status == success) {
                encoder.extract_chain(spec, chain);
                return success;
            } else if (status == failure) {
                total_conflicts += solver.nr_conflicts();
                if (spec.conflict_limit &&
                    total_conflicts > spec.conflict_limit) {
                    return timeout;
                }
                continue;
            } else {
                return timeout;
            }
        }
    }

    synth_result
    fence_synthesize(
        spec& spec, 
        chain& chain, 
        solver_wrapper& solver, 
        fence_encoder& encoder, 
        fence& fence)
    {
        solver.restart();
        if (!encoder.encode(spec, fence)) {
            return failure;
        }
        auto status = solver.solve(spec.conflict_limit);
        if (status == success) {
            encoder.extract_chain(spec, chain);
        }
        return status;
    }

    synth_result
    fence_cegar_synthesize(
        spec& spec, 
        chain& chain, 
        solver_wrapper& solver, 
        fence_encoder& encoder, 
        fence& fence)
    {
        spec.nr_rand_tt_assigns = 2 * spec.get_nr_in();
        solver.restart();
        if (!encoder.cegar_encode(spec, fence)) {
            return failure;
        }
        
        while (true) {
            auto status = solver.solve(spec.conflict_limit);
            if (status == success) {
                auto sim_tt = encoder.simulate(spec);
                if (spec.out_inv) {
                    sim_tt = ~sim_tt;
                }
                auto xor_tt = sim_tt ^ (spec[0]);
                auto first_one = kitty::find_first_one_bit(xor_tt);
                if (first_one == -1) {
                    encoder.extract_chain(spec, chain);
                    return success;
                }
                // Add additional constraint.
                if (spec.verbosity) {
                    printf("  CEGAR difference at tt index %ld\n",
                        first_one);
                }
                if (!encoder.create_tt_clauses(spec, first_one - 1)) {
                    return failure;
                }
            } else {
                return status;
            }
        }
    }
    
    synth_result 
    fence_cegar_synthesize(spec& spec, chain& chain, solver_wrapper& solver, fence_encoder& encoder)
    {
        assert(spec.get_nr_in() >= spec.fanin);

        spec.preprocess();
        encoder.set_dirty(true);

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            chain.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                chain.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        spec.nr_rand_tt_assigns = 2 * spec.get_nr_in();

        fence f;
        po_filter<unbounded_generator> g(
            unbounded_generator(spec.initial_steps),
            spec.get_nr_out(), spec.fanin);
        while (true) {
            g.next_fence(f);
            spec.nr_steps = f.nr_nodes();

            if (spec.verbosity) {
                printf("  next fence:\n");
                print_fence(f);
                printf("\n");
                printf("nr_nodes=%d, nr_levels=%d\n", f.nr_nodes(),
                    f.nr_levels());
                for (int i = 0; i < f.nr_levels(); i++) {
                    printf("f[%d] = %d\n", i, f[i]);
                }
            }

            solver.restart();
            if (!encoder.cegar_encode(spec, f)) {
                continue;
            }
            while (true) {
                auto status = solver.solve(spec.conflict_limit);
                if (status == success) {
                    auto sim_tt = encoder.simulate(spec);
                    if (spec.out_inv) {
                        sim_tt = ~sim_tt;
                    }
                    auto xor_tt = sim_tt ^ (spec[0]);
                    auto first_one = kitty::find_first_one_bit(xor_tt);
                    if (first_one == -1) {
                        encoder.extract_chain(spec, chain);
                        return success;
                    }
                    if (!encoder.create_tt_clauses(spec, first_one - 1)) {
                        break;
                    }
                } else if (status == failure) {
                    break;
                } else {
                    return timeout;
                }
            }
        }
    }

    ///< TODO: implement
    synth_result
    dag_synthesize(spec& spec, chain& chain, solver_wrapper& solver, dag_encoder<2>& encoder)
    {
        return failure;
    }

    synth_result 
    synthesize(
        spec& spec, 
        chain& chain, 
        solver_wrapper& solver, 
        encoder& encoder, 
        SynthMethod synth_method = SYNTH_STD,
        synth_stats * stats = NULL)
    {
        switch (synth_method) {
        case SYNTH_STD:
            return std_synthesize(spec, chain, solver, static_cast<std_encoder&>(encoder), stats);
        case SYNTH_STD_CEGAR:
            return std_cegar_synthesize(spec, chain, solver, static_cast<std_encoder&>(encoder), stats);
        case SYNTH_FENCE:
            return fence_synthesize(spec, chain, solver, static_cast<fence_encoder&>(encoder));
        case SYNTH_FENCE_CEGAR:
            return fence_cegar_synthesize(spec, chain, solver, static_cast<fence_encoder&>(encoder));
        case SYNTH_DAG:
            return dag_synthesize(spec, chain, solver, static_cast<dag_encoder<2>&>(encoder));
        default:
            fprintf(stderr, "Error: synthesis method %d not supported\n", synth_method);
            exit(1);
        }
    }

    synth_result
    pd_synthesize(
        spec& spec, 
        chain& chain, 
        const partial_dag& dag,
        solver_wrapper& solver, 
        partial_dag_encoder& encoder)
    {
        spec.nr_steps = dag.nr_vertices();
        solver.restart();
        if (!encoder.encode(spec, dag)) {
            return failure;
        }

        synth_result status;
        const auto begin = std::chrono::steady_clock::now();
        status = solver.solve(0);

        if (status == success) {
            encoder.extract_chain(spec, dag, chain);
            if (spec.verbosity > 2) {
                //    encoder.print_solver_state(spec);
            }
            return success;
        } else if (status == failure) {
            return failure;
        } else {
            return percy::synth_result::timeout;
        }
    }

    synth_result pd_cegar_synthesize(
        spec& spec, 
        chain& chain, 
        const partial_dag& dag,
        solver_wrapper& solver, 
        partial_dag_encoder& encoder)
    {
        spec.nr_rand_tt_assigns = 2 * spec.get_nr_in();
        spec.nr_steps = dag.nr_vertices();
        solver.restart();
        if (!encoder.cegar_encode(spec, dag)) {
            return failure;
        }
        while (true) {
            auto begin = std::chrono::steady_clock::now();
            auto stat = solver.solve(0);
            auto end = std::chrono::steady_clock::now();
            auto elapsed_time =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    end - begin
                    ).count();

            if (stat == success) {
                auto sim_tt = encoder.simulate(spec, dag);
                if (spec.out_inv) {
                    sim_tt = ~sim_tt;
                }
                auto xor_tt = sim_tt ^ (spec[0]);
                auto first_one = kitty::find_first_one_bit(xor_tt);
                if (first_one == -1) {
                    encoder.extract_chain(spec, dag, chain);
                    return success;
                }
                // Add additional constraint.
                if (spec.verbosity) {
                    printf("  CEGAR difference at tt index %ld\n",
                        first_one);
                }
                if (!encoder.create_tt_clauses(spec, dag, first_one - 1)) {
                    return failure;
                } else if (!encoder.fix_output_sim_vars(spec, first_one - 1)) {
                    return failure;
                }
            } else {
                return failure;
            }
        }
    }

    synth_result
    pd_synthesize_enum(
        spec& spec, 
        chain& chain, 
        const partial_dag& dag,
        synth_stats * stats = NULL)
    {
        partial_dag_generator gen;
        chain.reset(spec.get_nr_in(), 1, dag.nr_vertices(), 2);
        chain.set_output(0, (spec.get_nr_in() + dag.nr_vertices()) << 1);

        const auto found_sol = gen.search_sol(spec, chain, dag, 0);
        if (found_sol) {
            return success;
        } else {
            return failure;
        }
    }

    synth_result
    pd_synthesize_enum(
        spec& spec, 
        chain& chain, 
        const std::vector<partial_dag>& dags,
        synth_stats * stats = NULL)
    {
        partial_dag_generator gen;
        spec.preprocess();

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            chain.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                chain.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        for (const auto& dag : dags) {
            const auto result = 
                pd_synthesize_enum(spec, chain, dag, stats);
            if (result == success) {
                return success;
            }
        }

        return failure;
    }

    synth_result pd_synthesize(
        spec& spec,
        chain& chain,
        const std::vector<partial_dag>& dags,
        solver_wrapper& solver,
        partial_dag_encoder& encoder,
        SynthMethod synth_method = SYNTH_STD)
    {
        assert(spec.get_nr_in() >= spec.fanin);
        spec.preprocess();

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            chain.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                chain.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        for (auto& dag : dags) {
            synth_result status;
            switch (synth_method) {
            case SYNTH_STD_CEGAR:
                status = pd_cegar_synthesize(spec, chain, dag, solver, encoder);
                break;
            default:
                status = pd_synthesize(spec, chain, dag, solver, encoder);
                break;
            }
            if (status == success) {
                return success;
            }
        }
        return failure;
    }

    synth_result
    pd_synthesize_parallel(
        spec& spec, 
        chain& c, 
        const std::vector<partial_dag>& dags,
        int num_threads = std::thread::hardware_concurrency())
    {
        assert(spec.get_nr_in() >= spec.fanin);
        spec.preprocess();

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            c.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                c.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        std::vector<std::thread> threads(num_threads);

        moodycamel::ConcurrentQueue<partial_dag> q(num_threads * 3);

        bool finished_generating = false;
        bool* pfinished = &finished_generating;
        int size_found = PD_SIZE_CONST;
        int* psize_found = &size_found;
        std::mutex found_mutex;

        for (int i = 0; i < num_threads; i++) {
            threads[i] = std::thread([&spec, psize_found, pfinished, &found_mutex, &c, &q] {
                percy::spec local_spec = spec;
                bsat_wrapper solver;
                partial_dag_encoder encoder(solver);
                partial_dag dag;

                while (*psize_found > local_spec.nr_steps) {
                    if (!q.try_dequeue(dag)) {
                        if (*pfinished) {
                            std::this_thread::yield();
                            if (!q.try_dequeue(dag)) {
                                break;
                            }
                        } else {
                            std::this_thread::yield();
                            continue;
                        }
                    }
                    local_spec.nr_steps = dag.nr_vertices();
                    synth_result status;
                    solver.restart();
                    if (!encoder.encode(local_spec, dag)) {
                        continue;
                    }
                    while (true) {
                        status = solver.solve(10);
                        if (status == failure) {
                            break;
                        } else if (status == success) {
                            std::lock_guard<std::mutex> vlock(found_mutex);
                            if (*psize_found > local_spec.nr_steps) {
                                encoder.extract_chain(local_spec, dag, c);
                                *psize_found = local_spec.nr_steps;
                            }
                            break;
                        } else if (*psize_found <= local_spec.nr_steps) {
                            // Another thread found a solution that's
                            // better or equal to this one.
                            break;
                        }

                    }
                }
            });
        }
        size_t dag_idx = 0;
        while (size_found == PD_SIZE_CONST) {
            while (!q.try_enqueue(dags.at(dag_idx))) {
                if (size_found == PD_SIZE_CONST) {
                    std::this_thread::yield();
                } else {
                    break;
                }
            }
            dag_idx++;
        }
        finished_generating = true;
        for (auto& thread : threads) {
            thread.join();
        }

        return success;
    }


    /// Synthesizes a chain using a set of serialized partial DAGs.
    synth_result pd_ser_synthesize(
        spec& spec,
        chain& chain,
        solver_wrapper& solver,
        partial_dag_encoder& encoder,
        std::string file_prefix = "",
        int max_time = std::numeric_limits<int>::max()) // Timeout in seconds
    {
        assert(spec.get_nr_in() >= spec.fanin);
        spec.preprocess();

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            chain.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                chain.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        partial_dag g;
        spec.nr_steps = spec.initial_steps;
        auto begin = std::chrono::steady_clock::now();
        while (true) {
            g.reset(2, spec.nr_steps);
            const auto filename = file_prefix + "pd" + std::to_string(spec.nr_steps) + ".bin";
            auto fhandle = fopen(filename.c_str(), "rb");
            if (fhandle == NULL) {
                fprintf(stderr, "Error: unable to open file %s\n", filename.c_str());
                break;
            }

            int buf;
            while (fread(&buf, sizeof(int), 1, fhandle) != 0) {
                for (int i = 0; i < spec.nr_steps; i++) {
                    (void)fread(&buf, sizeof(int), 1, fhandle);
                    auto fanin1 = buf;
                    (void)fread(&buf, sizeof(int), 1, fhandle);
                    auto fanin2 = buf;
                    g.set_vertex(i, fanin1, fanin2);
                }
                solver.restart();
                if (!encoder.encode(spec, g)) {
                    continue;
                }
                const auto status = solver.solve(0);
                auto end = std::chrono::steady_clock::now();
                auto elapsed_time =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        end - begin
                        ).count();
                if (elapsed_time > max_time) {
                    return timeout;
                }
                if (status == success) {
                    encoder.extract_chain(spec, g, chain);
                    fclose(fhandle);
                    return success;
                }
            }
            fclose(fhandle);
            spec.nr_steps++;
        }

        return failure;
    }

    /// Same as pd_ser_synthesize, but parallel.  
    synth_result pd_ser_synthesize_parallel(
        spec& spec,
        chain& c,
        int num_threads = std::thread::hardware_concurrency(),
        std::string file_prefix ="")
    {
        assert(spec.get_nr_in() >= spec.fanin);
        spec.preprocess();

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            c.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                c.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        std::vector<std::thread> threads(num_threads);
        moodycamel::ConcurrentQueue<partial_dag> q(num_threads * 3);
        bool finished_generating = false;
        bool* pfinished = &finished_generating;
        int size_found = PD_SIZE_CONST;
        int* psize_found = &size_found;
        std::mutex found_mutex;

        for (int i = 0; i < num_threads; i++) {
            threads[i] = std::thread([&spec, psize_found, pfinished, &found_mutex, &c, &q] {
                percy::spec local_spec = spec;
                bsat_wrapper solver;
                partial_dag_encoder encoder(solver);
                partial_dag dag;

                while (*psize_found > local_spec.nr_steps) {
                    if (!q.try_dequeue(dag)) {
                        if (*pfinished) {
                            std::this_thread::yield();
                            if (!q.try_dequeue(dag)) {
                                break;
                            }
                        } else {
                            std::this_thread::yield();
                            continue;
                        }
                    }
                    local_spec.nr_steps = dag.nr_vertices();
                    synth_result status;
                    solver.restart();
                    if (!encoder.encode(local_spec, dag)) {
                        continue;
                    }
                    while (true) {
                        status = solver.solve(10);
                        if (status == failure) {
                            break;
                        } else if (status == success) {
                            std::lock_guard<std::mutex> vlock(found_mutex);
                            if (*psize_found > local_spec.nr_steps) {
                                encoder.extract_chain(local_spec, dag, c);
                                *psize_found = local_spec.nr_steps;
                            }
                            break;
                        } else if (*psize_found <= local_spec.nr_steps) {
                            // Another thread found a solution that's
                            // better or equal to this one.
                            break;
                        }
                    }
                }
            });
        }

        partial_dag g;
        spec.nr_steps = spec.initial_steps;
        while (size_found == PD_SIZE_CONST) {
            g.reset(2, spec.nr_steps);
            const auto filename = file_prefix + "pd" + std::to_string(spec.nr_steps) + ".bin";
            auto fhandle = fopen(filename.c_str(), "rb");
            if (fhandle == NULL) {
                fprintf(stderr, "Error: unable to open PD file\n");
                break;
            }

            int buf;
            while (fread(&buf, sizeof(int), 1, fhandle) != 0) {
                for (int i = 0; i < spec.nr_steps; i++) {
                    (void)fread(&buf, sizeof(int), 1, fhandle);
                    auto fanin1 = buf;
                    (void)fread(&buf, sizeof(int), 1, fhandle);
                    auto fanin2 = buf;
                    g.set_vertex(i, fanin1, fanin2);
                }
                while (!q.try_enqueue(g)) {
                    if (size_found == PD_SIZE_CONST) {
                        std::this_thread::yield();
                    } else {
                        break;
                    }
                }
                if (size_found != PD_SIZE_CONST) {
                    break;
                }
            }
            fclose(fhandle);
            spec.nr_steps++;
        }
        finished_generating = true;
        for (auto& thread : threads) {
            thread.join();
        }
        spec.nr_steps = size_found;

        return size_found == PD_SIZE_CONST ? failure : success;
    }
            
    synth_result
    pf_fence_synthesize(
        spec& spec, 
        chain& c, 
        int num_threads = std::thread::hardware_concurrency())
    {
        spec.preprocess();

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            c.reset(spec.get_nr_in(), spec.get_nr_out(), 0, spec.fanin);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                c.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        std::vector<std::thread> threads(num_threads);

        moodycamel::ConcurrentQueue<fence> q(num_threads * 3);

        bool finished_generating = false;
        bool* pfinished = &finished_generating;
        bool found = false;
        bool* pfound = &found;
        std::mutex found_mutex;

        spec.nr_steps = spec.initial_steps;
        while (true) {
            for (int i = 0; i < num_threads; i++) {
                threads[i] = std::thread([&spec, pfinished, pfound, &found_mutex, &c, &q] {
                    bsat_wrapper solver;
                    knuth_fence2_encoder encoder(solver);
                    fence local_fence;

                    while (!(*pfound)) {
                        if (!q.try_dequeue(local_fence)) {
                            if (*pfinished) {
                                std::this_thread::yield();
                                if (!q.try_dequeue(local_fence)) {
                                    break;
                                }
                            } else {
                                std::this_thread::yield();
                                continue;
                            }
                        }
                        synth_result status;
                        solver.restart();
                        if (!encoder.encode(spec, local_fence)) {
                            continue;
                        }
                        do {
                            status = solver.solve(10);
                            if (*pfound) {
                                break;
                            } else if (status == success) {
                                std::lock_guard<std::mutex> vlock(found_mutex);
                                if (!(*pfound)) {
                                    encoder.extract_chain(spec, c);
                                    *pfound = true;
                                }
                            }
                        } while (status == timeout);
                    }
                });
            }
            generate_fences(spec, q);
            finished_generating = true;

            for (auto& thread : threads) {
                thread.join();
            }
            if (found) {
                break;
            }
            finished_generating = false;
            spec.nr_steps++;
        }

        return success;
    }
    
    /// Performs fence-based parallel synthesis.
    /// One thread generates fences and places them on a concurrent
    /// queue. The remaining threads dequeue fences and try to
    /// synthesize chains with them.
    synth_result
    pf_synthesize(
        spec& spec, 
        chain&  chain, 
        SynthMethod synth_method = SYNTH_FENCE)
    {
        switch (synth_method) {
        case SYNTH_FENCE:
            return pf_fence_synthesize(spec, chain);
//        case SYNTH_FENCE_CEGAR:
//            return pf_fence_cegar_synthesize(spec, chain, solver, encoder);
        default:
            fprintf(stderr, "Error: synthesis method %d not supported\n", synth_method);
            exit(1);
        }
    }

    synth_result
    synthesize(
        spec& spec, 
        chain& chain, 
        SolverType slv_type = SLV_BSAT2, 
        EncoderType enc_type = ENC_KNUTH, 
        SynthMethod method = SYNTH_STD)
    {
        auto solver = get_solver(slv_type);
        auto encoder = get_encoder(*solver, enc_type);
        return synthesize(spec, chain, *solver, *encoder, method);
    }

    synth_result
    next_solution(
        spec& spec, 
        chain& chain, 
        solver_wrapper& solver, 
        enumerating_encoder& encoder, 
        SynthMethod synth_method = SYNTH_STD)
    {
        if (!encoder.is_dirty()) {
            switch (synth_method) {
            case SYNTH_STD:
            case SYNTH_STD_CEGAR:
                return std_synthesize(spec, chain, solver, static_cast<std_encoder&>(encoder));
            case SYNTH_FENCE:
                return fence_synthesize(spec, chain, solver, static_cast<fence_encoder&>(encoder));
            default:
                fprintf(stderr, "Error: solution enumeration not supported for synth method %d\n", synth_method);
                exit(1);
            }
        }

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        // In this case, only one solution exists.
        if (spec.nr_triv == spec.get_nr_out()) {
            return failure;
        }

        if (encoder.block_solution(spec)) {
            const auto status = solver.solve(spec.conflict_limit);

            if (status == success) {
                encoder.extract_chain(spec, chain);
                return success;
            } else {
                return status;
            }
        }

        return failure;
    }

    synth_result
    next_struct_solution(
        spec& spec, 
        chain& chain, 
        solver_wrapper& solver, 
        enumerating_encoder& encoder,
        SynthMethod synth_method = SYNTH_STD)
    {
        if (!encoder.is_dirty()) {
            switch (synth_method) {
            case SYNTH_STD:
            case SYNTH_STD_CEGAR:
                return std_synthesize(spec, chain, solver, static_cast<std_encoder&>(encoder));
            case SYNTH_FENCE:
                return fence_synthesize(spec, chain, solver, static_cast<fence_encoder&>(encoder));
            default:
                fprintf(stderr, "Error: solution enumeration not supported for synth method %d\n", synth_method);
                exit(1);
            }
        }

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        // In this case, only one solution exists.
        if (spec.nr_triv == spec.get_nr_out()) {
            return failure;
        }

        if (encoder.block_solution(spec)) {
            const auto status = solver.solve(spec.conflict_limit);

            if (status == success) {
                encoder.extract_chain(spec, chain);
                return success;
            } else {
                return status;
            }
        }

        return failure;
    }

    synth_result
    maj_synthesize(
        spec& spec, 
        mig& mig, 
        solver_wrapper& solver, 
        maj_encoder& encoder)
    {
        spec.preprocess();
        encoder.set_dirty(true);

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            mig.reset(spec.get_nr_in(), spec.get_nr_out(), 0);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                mig.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        spec.nr_steps = spec.initial_steps;
        while (true) {
            solver.restart();
            if (!encoder.encode(spec)) {
                spec.nr_steps++;
                continue;
            }

            const auto status = solver.solve(spec.conflict_limit);

            if (status == success) {
                //encoder.print_solver_state(spec);
                encoder.extract_mig(spec, mig);
                return success;
            } else if (status == failure) {
                spec.nr_steps++;
            } else {
                return timeout;
            }
        }
    }

    synth_result
    maj_cegar_synthesize(
        spec& spec, 
        mig& mig, 
        solver_wrapper& solver, 
        maj_encoder& encoder)
    {
        spec.preprocess();
        encoder.set_dirty(true);

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            mig.reset(spec.get_nr_in(), spec.get_nr_out(), 0);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                mig.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        spec.nr_steps = spec.initial_steps;
        while (true) {
            solver.restart();
            if (!encoder.encode(spec)) {
                spec.nr_steps++;
                continue;
            }
            while (true) {
                const auto status = solver.solve(spec.conflict_limit);

                if (status == success) {
                    encoder.extract_mig(spec, mig);
                    auto sim_tt = mig.simulate()[0];
                    auto xor_tt = sim_tt ^ (spec[0]);
                    auto first_one = kitty::find_first_one_bit(xor_tt);
                    if (first_one == -1) {
                        return success;
                    }
                    if (!encoder.create_tt_clauses(spec, first_one - 1)) {
                        spec.nr_steps++;
                        break;
                    }
                } else if (status == failure) {
                    spec.nr_steps++;
                    break;
                } else {
                    return timeout;
                }
            }
        }
    }

    synth_result
    maj_fence_synthesize(spec& spec, mig& mig, solver_wrapper& solver, maj_encoder& encoder)
    {
        spec.preprocess();
        encoder.set_dirty(true);

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            mig.reset(spec.get_nr_in(), spec.get_nr_out(), 0);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                mig.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        // As the topological synthesizer decomposes the synthesis
        // problem, to fairly count the total number of conflicts we
        // should keep track of all conflicts in existence checks.
        fence f;
        po_filter<unbounded_generator> g(
            unbounded_generator(spec.initial_steps),
            spec.get_nr_out(), 3);
        auto fence_ctr = 0;
        while (true) {
            ++fence_ctr;
            g.next_fence(f);
            spec.nr_steps = f.nr_nodes();
            solver.restart();
            if (!encoder.encode(spec, f)) {
                continue;
            }

            if (spec.verbosity) {
                printf("next fence (%d):\n", fence_ctr);
                print_fence(f);
                printf("\n");
                printf("nr_nodes=%d, nr_levels=%d\n", f.nr_nodes(),
                    f.nr_levels());
                for (int i = 0; i < f.nr_levels(); i++) {
                    printf("f[%d] = %d\n", i, f[i]);
                }
            }
            auto status = solver.solve(spec.conflict_limit);
            if (status == success) {
                encoder.fence_extract_mig(spec, mig);
                //encoder.fence_print_solver_state(spec);
                return success;
            } else if (status == failure) {
                continue;
            } else {
                return timeout;
            }
        }
    }

    synth_result maj_fence_cegar_synthesize(
        spec& spec, 
        mig& mig, 
        solver_wrapper& solver, 
        maj_encoder& encoder)
    {
        assert(spec.get_nr_in() >= spec.fanin);

        spec.preprocess();
        encoder.set_dirty(true);

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            mig.reset(spec.get_nr_in(), spec.get_nr_out(), 0);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                mig.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        spec.nr_rand_tt_assigns = 2 * spec.get_nr_in();

        fence f;
        po_filter<unbounded_generator> g(
            unbounded_generator(spec.initial_steps),
            spec.get_nr_out(), 3);
        int fence_ctr = 0;
        while (true) {
            ++fence_ctr;
            g.next_fence(f);
            spec.nr_steps = f.nr_nodes();

            if (spec.verbosity) {
                printf("  next fence (%d):\n", fence_ctr);
                print_fence(f);
                printf("\n");
                printf("nr_nodes=%d, nr_levels=%d\n", f.nr_nodes(),
                    f.nr_levels());
                for (int i = 0; i < f.nr_levels(); i++) {
                    printf("f[%d] = %d\n", i, f[i]);
                }
            }

            solver.restart();
            if (!encoder.cegar_encode(spec, f)) {
                continue;
            }
            while (true) {
                auto status = solver.solve(spec.conflict_limit);
                if (status == success) {
                    encoder.fence_extract_mig(spec, mig);
                    auto sim_tt = mig.simulate()[0];
                    //auto sim_tt = encoder.simulate(spec);
                    //if (spec.out_inv) {
                    //    sim_tt = ~sim_tt;
                    //}
                    auto xor_tt = sim_tt ^ (spec[0]);
                    auto first_one = kitty::find_first_one_bit(xor_tt);
                    if (first_one == -1) {
                        return success;
                    }
                    if (!encoder.fence_create_tt_clauses(spec, first_one - 1)) {
                        break;
                    }
                } else if (status == failure) {
                    break;
                } else {
                    return timeout;
                }
            }
        }
    }

    synth_result
    parallel_maj_synthesize(
        spec& spec, 
        mig& mig, 
        int num_threads = std::thread::hardware_concurrency())
    {
        spec.preprocess();

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            mig.reset(spec.get_nr_in(), spec.get_nr_out(), 0);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                mig.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        std::vector<std::thread> threads(num_threads);
        moodycamel::ConcurrentQueue<fence> q(num_threads * 3);

        bool finished_generating = false;
        bool* pfinished = &finished_generating;
        bool found = false;
        bool* pfound = &found;
        std::mutex found_mutex;

        spec.nr_rand_tt_assigns = 2 * spec.get_nr_in();
        spec.fanin = 3;
        spec.nr_steps = spec.initial_steps;
        while (true) {
            for (int i = 0; i < num_threads; i++) {
                threads[i] = std::thread([&spec, pfinished, pfound, &found_mutex, &mig, &q] {
                    percy::mig local_mig;
                    bsat_wrapper solver;
                    maj_encoder encoder(solver);
                    fence local_fence;

                    while (!(*pfound)) {
                        if (!q.try_dequeue(local_fence)) {
                            if (*pfinished) {
                                std::this_thread::yield();
                                if (!q.try_dequeue(local_fence)) {
                                    break;
                                }
                            } else {
                                std::this_thread::yield();
                                continue;
                            }
                        }

                        if (spec.verbosity)
                        {
                            std::lock_guard<std::mutex> vlock(found_mutex);
                            printf("  next fence:\n");
                            print_fence(local_fence);
                            printf("\n");
                            printf("nr_nodes=%d, nr_levels=%d\n",
                                local_fence.nr_nodes(),
                                local_fence.nr_levels());
                        }

                        synth_result status;
                        solver.restart();
                        if (!encoder.cegar_encode(spec, local_fence)) {
                            continue;
                        }
                        do {
                            status = solver.solve(10);
                            if (*pfound) {
                                break;
                            } else if (status == success) {
                                encoder.fence_extract_mig(spec, local_mig);
                                auto sim_tt = local_mig.simulate()[0];
                                //auto sim_tt = encoder.simulate(spec);
                                //if (spec.out_inv) {
                                //    sim_tt = ~sim_tt;
                                //}
                                auto xor_tt = sim_tt ^ (spec[0]);
                                auto first_one = kitty::find_first_one_bit(xor_tt);
                                if (first_one != -1) {
                                    if (!encoder.fence_create_tt_clauses(spec, first_one - 1)) {
                                        break;
                                    }
                                    status = timeout;
                                    continue;
                                }
                                std::lock_guard<std::mutex> vlock(found_mutex);
                                if (!(*pfound)) {
                                    encoder.fence_extract_mig(spec, mig);
                                    *pfound = true;
                                }
                            }
                        } while (status == timeout);
                    }
                });
            }
            generate_fences(spec, q);
            finished_generating = true;

            for (auto& thread : threads) {
                thread.join();
            }
            if (found) {
                break;
            }
            finished_generating = false;
            spec.nr_steps++;
        }

        return success;
    }

    synth_result
    parallel_nocegar_maj_synthesize(
        spec& spec, 
        mig& mig, 
        int num_threads = std::thread::hardware_concurrency())
    {
        spec.preprocess();

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        if (spec.nr_triv == spec.get_nr_out()) {
            mig.reset(spec.get_nr_in(), spec.get_nr_out(), 0);
            for (int h = 0; h < spec.get_nr_out(); h++) {
                mig.set_output(h, (spec.triv_func(h) << 1) +
                    ((spec.out_inv >> h) & 1));
            }
            return success;
        }

        std::vector<std::thread> threads(num_threads);
        moodycamel::ConcurrentQueue<fence> q(num_threads * 3);

        bool finished_generating = false;
        bool* pfinished = &finished_generating;
        bool found = false;
        bool* pfound = &found;
        std::mutex found_mutex;

        spec.nr_rand_tt_assigns = 2 * spec.get_nr_in();
        spec.fanin = 3;
        spec.nr_steps = spec.initial_steps;
        while (true) {
            for (int i = 0; i < num_threads; i++) {
                threads[i] = std::thread([&spec, pfinished, pfound, &found_mutex, &mig, &q] {
                    bsat_wrapper solver;
                    maj_encoder encoder(solver);
                    fence local_fence;

                    while (!(*pfound)) {
                        if (!q.try_dequeue(local_fence)) {
                            if (*pfinished) {
                                std::this_thread::yield();
                                if (!q.try_dequeue(local_fence)) {
                                    break;
                                }
                            } else {
                                std::this_thread::yield();
                                continue;
                            }
                        }

                        if (spec.verbosity)
                        {
                            std::lock_guard<std::mutex> vlock(found_mutex);
                            printf("  next fence:\n");
                            print_fence(local_fence);
                            printf("\n");
                            printf("nr_nodes=%d, nr_levels=%d\n",
                                local_fence.nr_nodes(),
                                local_fence.nr_levels());
                        }

                        synth_result status;
                        solver.restart();
                        if (!encoder.encode(spec, local_fence)) {
                            continue;
                        }
                        do {
                            status = solver.solve(10);
                            if (*pfound) {
                                break;
                            } else if (status == success) {
                                std::lock_guard<std::mutex> vlock(found_mutex);
                                if (!(*pfound)) {
                                    encoder.fence_extract_mig(spec, mig);
                                    *pfound = true;
                                }
                            }
                        } while (status == timeout);
                    }
                });
            }
            generate_fences(spec, q);
            finished_generating = true;

            for (auto& thread : threads) {
                thread.join();
            }
            if (found) {
                break;
            }
            finished_generating = false;
            spec.nr_steps++;
        }

        return success;
    }


    synth_result
    next_solution(
        spec& spec,
        mig& mig,
        solver_wrapper& solver,
        maj_encoder& encoder)
    {
        if (!encoder.is_dirty()) {
            return maj_synthesize(spec, mig, solver, encoder);
        }

        // The special case when the Boolean chain to be synthesized
        // consists entirely of trivial functions.
        // In this case, only one solution exists.
        if (spec.nr_triv == spec.get_nr_out()) {
            return failure;
        }

        if (encoder.block_solution(spec)) {
            const auto status = solver.solve(spec.conflict_limit);

            if (status == success) {
                encoder.extract_mig(spec, mig);
                return success;
            } else {
                return status;
            }
        }

        return failure;
    }
}

