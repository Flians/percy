#include <percy/percy.hpp>
#include <kitty/kitty.hpp>
#include <cassert>
#include <cstdio>
#include <fstream>

using namespace percy;
using kitty::static_truth_table;

/*******************************************************************************
    Tests the exact synthesis functionality of the package.
*******************************************************************************/
int main(void)
{
    
    {
        synth_spec<static_truth_table<2>> spec;
        spec.set_nr_in(2);
        spec.set_nr_out(1);
        spec.verbosity = 1;
        auto synth = new_std_synth();
        chain<2> c;

        static_truth_table<2> tti;
        static_truth_table<2> ttj;

        for (int i = 0; i < 16; i++) {
            kitty::create_from_words(tti, &i, &i+1);
            spec.functions[0] = &tti;
            synth->synthesize<static_truth_table<2>>(spec, c);
            auto sim_fs = c.simulate<static_truth_table<2>>();
            assert(c.get_nr_vertices() <= 1);
            assert(sim_fs[0] == tti);
        }

        spec.set_nr_out(2);
        for (int i = 0; i < 16; i++) {
            kitty::create_from_words(tti, &i, &i+1);
            spec.functions[0] = &tti;
            for (int j = 0; j < 16; j++) {
                kitty::create_from_words(ttj, &j, &j+1);
                spec.functions[1] = &ttj;
                auto result = synth->synthesize(spec, c);
                assert(result == success);
                auto sim_fs = c.simulate<static_truth_table<2>>();
                assert(c.get_nr_vertices() <= 2);
                assert(sim_fs[0] == tti);
                assert(sim_fs[1] == ttj);
            }
        }
    }
    
    // Synthesize a full adder
    {
        synth_spec<static_truth_table<3>> spec(3, 2);
        spec.verbosity = 1;
        auto synth = new_std_synth();
        chain<2> c;

        static_truth_table<3> x, y, z;

        create_nth_var( x, 0 );
        create_nth_var( y, 1 );
        create_nth_var( z, 2 );

        const auto sum = x ^ y ^ z;
        const auto carry = ternary_majority(x, y, z);
        spec.functions[0] = &sum;
        spec.functions[1] = &carry;
        auto result = synth->synthesize(spec, c);
        assert(result == success);
        auto sim_fs = c.simulate<static_truth_table<3>>();
        assert(sim_fs[0] == sum);
        assert(sim_fs[1] == carry);
        /*
        {
            std::ofstream dotfile("full_adder.dot");
            c.to_dot(dotfile);
        }
        */
    }

    {
      /*
       * arbiter example
       *
       * inputs:
       * - request by client #1: r1
       * - request by client #2: r2
       * - two state bits for the current state: s2 s1
       *
       * outputs:
       * - access granted to client #1: g1
       * - access granted to client #2: g2
       * - two state bits for the next state: s2_next, s1_next
       */

      /* truth tables of the grant siganls g1 and g2 of a round robin arbiter */
      const std::string g2_tt = "0000" "0000" "1111" "0100";
      const std::string g1_tt = "0000" "1111" "0000" "1010";

      chain<2> c;

      /* enumerate mutants *
      for ( auto i = 4u; i < g1_tt.size(); ++i )
      {
        synth_spec<static_truth_table<4>,sat_solver*> spec;
        spec.nr_in = 4;
        spec.nr_out = 4;
        spec.verbosity = 1;
        // simple_synthesizer<static_truth_table<4>,sat_solver*> synth;
        symmetric_synthesizer<static_truth_table<4>,sat_solver*> synth;

        static_truth_table<4> s2, s1, r2, r1, s2_next, s1_next, g2, g1;
        create_nth_var( r1, 0 );
        create_nth_var( r2, 1 );
        create_nth_var( s1, 2 );
        create_nth_var( s2, 3 );

        /* copy the output signal *
        auto g2_tt_copy = g2_tt;
        auto g1_tt_copy = g1_tt;

        /*
         * inject errors into the output signals (violates the
         * property never two grants at the same time)
         *
        g1_tt_copy[i] = '1';
        g2_tt_copy[i] = '1';

        /* next-state logic *
        create_from_binary_string( s2_next, "1111" "0000" "1010" "0000" );
        create_from_binary_string( s1_next, "1111" "1100" "0000" "1000" );

        /* output logic *
        create_from_binary_string( g2, g2_tt_copy );
        create_from_binary_string( g1, g1_tt_copy );

        spec.functions[0] = &s2_next;
        spec.functions[1] = &s1_next;
        spec.functions[2] = &g2;
        spec.functions[3] = &g1;

        synth.synthesize(spec, c);
        auto sim_fs = c.simulate();
        assert( sim_fs[0] == s2_next );
        assert( sim_fs[1] == s1_next );
        assert( sim_fs[2] == g2 );
        assert( sim_fs[3] == g1 );
      }
      */
    }

    return 0;
}

