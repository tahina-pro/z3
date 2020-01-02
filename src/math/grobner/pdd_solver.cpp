/*++
  Copyright (c) 2019 Microsoft Corporation

  Abstract:

    Solver core based on pdd representation of polynomials

  Author:
     Nikolaj Bjorner (nbjorner)
     Lev Nachmanson (levnach)

  --*/

#include "math/grobner/pdd_solver.h"
#include "math/grobner/pdd_simplifier.h"
#include "util/uint_set.h"


namespace dd {

    /***
        A simple algorithm maintains two sets (S, A), 
        where S is m_processed, and A is m_to_simplify.
        Initially S is empty and A contains the initial equations.
        
        Each step proceeds as follows:
        - pick a in A, and remove a from A
        - simplify a using S
        - simplify S using a
        - for s in S:
             b = superpose(a, s)
             add b to A
        - add a to S
        - simplify A using a

        Alternate:

        - Fix a variable ordering x1 > x2 > x3 > ....
        In each step:
          - pick a in A with *highest* variable x_i in leading term of *lowest* degree.
          - simplify a using S
          - simplify S using a
          - if a does not contains x_i, put it back into A and pick again (determine whether possible)
          - for s in S:
               b = superpose(a, s)
               add b to A
          - add a to S
          - simplify A using a


        Apply a watch list to filter out relevant elements of S
        Index leading_term_watch: Var -> Equation*
        Only need to simplify equations that contain eliminated variable.
        The watch list can be used to index into equations that are useful to simplify.
        A Bloom filter on leading term could further speed up test whether reduction applies.

        For p in A:
            populate watch list by maxvar(p) |-> p
        For p in S:
            do not occur in watch list

        - the variable ordering should be chosen from a candidate model M, 
          in a way that is compatible with weights that draw on the number of occurrences
          in polynomials with violated evaluations and with the maximal slack (degree of freedom).
         
          weight(x) := < count { p in P | x in p, M(p) != 0 }, min_{p in P | x in p} slack(p,x) >

          slack is computed from interval assignments to variables, and is an interval in which x can possibly move
          (an over-approximation).

        The alternative version maintains the following invariant:
        - polynomials not in the watch list cannot be simplified using a
          Justification:
          - elements in S have no variables watched
          - elements in A are always reduced modulo all variables above the current x_i.
       
    */

    solver::solver(reslimit& lim, pdd_manager& m) : 
        m(m),
        m_limit(lim), 
        m_conflict(nullptr)
    {}

    solver::~solver() {
        reset();
    }

    void solver::saturate() {
        simplify();
        init_saturate();
        TRACE("dd.solver", display(tout););
        try {
            while (!done() && step()) {
                TRACE("dd.solver", display(tout););
                DEBUG_CODE(invariant(););
                IF_VERBOSE(3, display_statistics(verbose_stream()));
            }
            DEBUG_CODE(invariant(););
        }
        catch (pdd_manager::mem_out) {
            m_watch.reset();
            IF_VERBOSE(2, verbose_stream() << "mem-out\n");
            // don't reduce further
        }
    }

    void solver::scoped_process::done() {
        pdd p = e->poly();
        SASSERT(!p.is_val());
        if (p.hi().is_val()) {
            g.push_equation(solved, e);
        }
        else {
            g.push_equation(processed, e);
        }
        e = nullptr;
    }

    solver::scoped_process::~scoped_process() {
        if (e) {
            pdd p = e->poly();
            SASSERT(!p.is_val());
            g.push_equation(processed, e);            
        }
    }    

    void solver::simplify() {
        simplifier s(*this);
        s();
    }


    void solver::superpose(equation const & eq) {
        for (equation* target : m_processed) {
            superpose(eq, *target);
        }
    }

    /*
      Use a set of equations to simplify eq
    */
    void solver::simplify_using(equation& eq, equation_vector const& eqs) {
        bool simplified, changed_leading_term;
        do {
            simplified = false;
            for (equation* p : eqs) {
                if (try_simplify_using(eq, *p, changed_leading_term)) {
                    simplified = true;
                }
                if (canceled() || eq.poly().is_val()) {
                    break;
                }
            }
        }
        while (simplified && !eq.poly().is_val());

        TRACE("dd.solver", display(tout << "simplification result: ", eq););
    }

    /*
      Use the given equation to simplify equations in set
    */
    void solver::simplify_using(equation_vector& set, equation const& eq) {
        
        struct scoped_update {
            equation_vector& set;
            unsigned i, j, sz;
            scoped_update(equation_vector& set): set(set), i(0), j(0), sz(set.size()) {}
            void nextj() {
                set[j] = set[i];
                set[i]->set_index(j++);
            }
            ~scoped_update() {                
                for (; i < sz; ++i) {
                    nextj();
                }
                set.shrink(j);
            }
        };

        scoped_update sr(set);
        for (; sr.i < sr.sz; ++sr.i) {
            equation& target = *set[sr.i];
            bool changed_leading_term = false;
            bool simplified = true;
            simplified = !done() && try_simplify_using(target, eq, changed_leading_term); 
            
            if (simplified && is_trivial(target)) {
                retire(&target);
            }
            else if (simplified && check_conflict(target)) {
                // pushed to solved
            }
            else if (simplified && changed_leading_term) {
                SASSERT(target.state() == processed);
                push_equation(to_simplify, target);
                if (!m_watch.empty()) {
                    m_levelp1 = std::max(m_var2level[target.poly().var()]+1, m_levelp1);
                    add_to_watch(target);
                }
            }
            else {
                sr.nextj();
            } 
        }
    }    

    /*
      simplify target using source.
      return true if the target was simplified. 
      set changed_leading_term if the target is in the m_processed set and the leading term changed.
     */
    bool solver::try_simplify_using(equation& dst, equation const& src, bool& changed_leading_term) {
        if (&src == &dst) {
            return false;
        }
        m_stats.m_simplified++;
        pdd t = src.poly();
        pdd r = dst.poly().reduce(t);
        if (r == dst.poly()){
            return false;
        }
        if (is_too_complex(r)) {
            m_too_complex = true;
            return false;
        }
        TRACE("dd.solver", 
              tout << "reduce: " << dst.poly() << "\n";
              tout << "using:  " << t << "\n";
              tout << "to:     " << r << "\n";);
        changed_leading_term = dst.state() == processed && m.different_leading_term(r, dst.poly());
        dst = r;
        dst = m_dep_manager.mk_join(dst.dep(), src.dep());
        update_stats_max_degree_and_size(dst);
        return true;
    }

    void solver::simplify_using(equation & dst, equation const& src, bool& changed_leading_term) {
        if (&src == &dst) return;        
        m_stats.m_simplified++;
        pdd t = src.poly();
        pdd r = dst.poly().reduce(t);
        changed_leading_term = dst.state() == processed && m.different_leading_term(r, dst.poly());
        TRACE("dd.solver", 
              tout << "reduce: " << dst.poly() << "\n";
              tout << "using:  " << t << "\n";
              tout << "to:     " << r << "\n";);

        if (r == dst.poly()) return;
        dst = r;
        dst = m_dep_manager.mk_join(dst.dep(), src.dep());
        update_stats_max_degree_and_size(dst);
    }

    /*
       let eq1: ab+q=0, and eq2: ac+e=0, then qc - eb = 0
    */
    void solver::superpose(equation const& eq1, equation const& eq2) {
        TRACE("dd.solver_d", display(tout << "eq1=", eq1); display(tout << "eq2=", eq2););
        pdd r(m);
        if (m.try_spoly(eq1.poly(), eq2.poly(), r) && !r.is_zero()) {
            if (is_too_complex(r)) {
                m_too_complex = true;
            }
            else {
                m_stats.m_superposed++;
                add(r, m_dep_manager.mk_join(eq1.dep(), eq2.dep()));
            }
        }
    }

    bool solver::step() {
        m_stats.m_compute_steps++;
        equation* e = pick_next();
        if (!e) return false;
        scoped_process sd(*this, e);
        equation& eq = *e;
        SASSERT(!m_watch[eq.poly().var()].contains(e));
        SASSERT(eq.state() == to_simplify);
        simplify_using(eq, m_processed);
        if (is_trivial(eq)) { sd.e = nullptr; retire(e); return true; }
        if (check_conflict(eq)) { sd.e = nullptr; return false; }
        m_too_complex = false;
        simplify_using(m_processed, eq);
        if (done()) return false;
        TRACE("dd.solver", display(tout << "eq = ", eq););
        superpose(eq);
        simplify_watch(eq);
        if (done()) return false;
        if (!m_too_complex) sd.done();
        return true;
    }

    void solver::init_saturate() {
        unsigned_vector const& l2v = m.get_level2var();
        m_level2var.resize(l2v.size());
        m_var2level.resize(l2v.size());
        for (unsigned i = 0; i < l2v.size(); ++i) {
            m_level2var[i] = l2v[i];
            m_var2level[l2v[i]] = i;
        }
        m_watch.reset();
        m_watch.reserve(m_level2var.size());
        m_levelp1 = m_level2var.size();
        for (equation* eq : m_to_simplify) add_to_watch(*eq);
    }

    void solver::add_to_watch(equation& eq) {
        SASSERT(eq.state() == to_simplify);
        pdd const& p = eq.poly();
        if (!p.is_val()) {
            m_watch[p.var()].push_back(&eq);
        }
    }

    void solver::simplify_watch(equation const& eq) {
        unsigned v = eq.poly().var();
        auto& watch = m_watch[v];
        unsigned j = 0;
        for (equation* _target : watch) {
            equation& target = *_target;
            SASSERT(target.state() == to_simplify);
            SASSERT(target.poly().var() == v);
            bool changed_leading_term = false;
            if (!done()) {
                try_simplify_using(target, eq, changed_leading_term);
            }
            if (is_trivial(target)) {
                pop_equation(target);
                retire(&target);
            }
            else if (is_conflict(target)) {
                pop_equation(target);
                set_conflict(target);
            }
            else if (target.poly().var() != v) {
                // move to other watch list
                m_watch[target.poly().var()].push_back(_target);
            }
            else {
                // keep watching same variable
                watch[j++] = _target;
            }
        }
        watch.shrink(j);        
    }

    solver::equation* solver::pick_next() {
        while (m_levelp1 > 0) {
            unsigned v = m_level2var[m_levelp1-1];
            equation_vector const& watch = m_watch[v];
            equation* eq = nullptr;
            for (equation* curr : watch) {
                pdd const& p = curr->poly();
                if (curr->state() == to_simplify && p.var() == v) {
                    if (!eq || is_simpler(*curr, *eq))
                        eq = curr;
                }
            }
            if (eq) {
                pop_equation(eq);
                m_watch[eq->poly().var()].erase(eq);
                return eq;
            }
            --m_levelp1;
        }
        return nullptr;
    }

    solver::equation_vector const& solver::equations() {
        m_all_eqs.reset();
        for (equation* eq : m_solved) m_all_eqs.push_back(eq);
        for (equation* eq : m_to_simplify) m_all_eqs.push_back(eq);
        for (equation* eq : m_processed) m_all_eqs.push_back(eq);
        return m_all_eqs;
    }

    void solver::reset() {
        for (equation* e : m_solved) dealloc(e);
        for (equation* e : m_to_simplify) dealloc(e);
        for (equation* e : m_processed) dealloc(e);
        m_solved.reset();
        m_processed.reset();
        m_to_simplify.reset();
        m_stats.reset();
        m_watch.reset();
        m_level2var.reset();
        m_var2level.reset();
        m_conflict = nullptr;
    }

    void solver::add(pdd const& p, u_dependency * dep) {
        if (p.is_zero()) return;
        equation * eq = alloc(equation, p, dep);
        if (check_conflict(*eq)) {
            return;
        }
        push_equation(to_simplify, eq);
        
        if (!m_watch.empty()) {
            m_levelp1 = std::max(m_var2level[p.var()]+1, m_levelp1);
            add_to_watch(*eq);
        }
        update_stats_max_degree_and_size(*eq);
    }   
    
    bool solver::canceled() {
        return m_limit.get_cancel_flag();
    }
    
    bool solver::done() {
        return 
            m_to_simplify.size() + m_processed.size() >= m_config.m_eqs_threshold || 
            canceled() || 
            m_stats.m_compute_steps > m_config.m_max_steps ||
            m_conflict != nullptr;
    }

    solver::equation_vector& solver::get_queue(equation const& eq) {
        switch (eq.state()) {
        case processed: return m_processed;
        case to_simplify: return m_to_simplify;
        case solved: return m_solved;
        }
        UNREACHABLE();
        return m_to_simplify;
    }

    void solver::del_equation(equation* eq) {
        pop_equation(eq);
        retire(eq);
    }

    void solver::pop_equation(equation& eq) {
        equation_vector& v = get_queue(eq);
        unsigned idx = eq.idx();
        if (idx != v.size() - 1) {
            equation* eq2 = v.back();
            eq2->set_index(idx);
            v[idx] = eq2;
        }
        v.pop_back();            
    }

    void solver::push_equation(eq_state st, equation& eq) {
        SASSERT(st != to_simplify || !eq.poly().is_val());
        SASSERT(st != processed || !eq.poly().is_val());
        eq.set_state(st);
        equation_vector& v = get_queue(eq);
        eq.set_index(v.size());
        v.push_back(&eq);
    }
    
    void solver::update_stats_max_degree_and_size(const equation& e) {
        m_stats.m_max_expr_size = std::max(m_stats.m_max_expr_size, e.poly().tree_size());
        m_stats.m_max_expr_degree = std::max(m_stats.m_max_expr_degree, e.poly().degree());
    }
    
    void solver::collect_statistics(statistics& st) const {
        st.update("dd.solver.steps", m_stats.m_compute_steps);
        st.update("dd.solver.simplified", m_stats.m_simplified);
        st.update("dd.solver.superposed", m_stats.m_superposed);
        st.update("dd.solver.processed", m_processed.size());
        st.update("dd.solver.solved", m_solved.size());
        st.update("dd.solver.to_simplify", m_to_simplify.size());
        st.update("dd.solver.degree", m_stats.m_max_expr_degree);
        st.update("dd.solver.size", m_stats.m_max_expr_size);
    }
            
    std::ostream& solver::display(std::ostream & out, const equation & eq) const {
        out << eq.poly() << "\n";
        if (m_print_dep) m_print_dep(eq.dep(), out);
        return out;
    }

    std::ostream& solver::display(std::ostream& out) const {
        out << "solved\n"; for (auto e : m_solved) display(out, *e);
        out << "processed\n"; for (auto e : m_processed) display(out, *e);
        out << "to_simplify\n"; for (auto e : m_to_simplify) display(out, *e);
        return display_statistics(out);
    }

    std::ostream& solver::display_statistics(std::ostream& out) const {
        statistics st;
        collect_statistics(st);
        st.display(out);
        return out;
    }

    void solver::invariant() const {
        // equations in processed have correct indices
        // they are labled as processed
        unsigned i = 0;
        for (auto* e : m_processed) {
            VERIFY(e->state() == processed);
            VERIFY(e->idx() == i);
            VERIFY(!e->poly().is_val());
            ++i;
        }

        i = 0;
        uint_set head_vars;
        for (auto* e : m_solved) {
            VERIFY(e->state() == solved);
            VERIFY(e->idx() == i);
            ++i;
            pdd p = e->poly();
            if (!p.is_val() && p.hi().is_val()) {
                unsigned v = p.var();
                SASSERT(!head_vars.contains(v));
                head_vars.insert(v);
            }
        }

        if (!head_vars.empty()) {
            for (auto * e : m_to_simplify) {
                for (auto v : m.free_vars(e->poly())) VERIFY(!head_vars.contains(v));
            }
            for (auto * e : m_processed) {
                for (auto v : m.free_vars(e->poly())) VERIFY(!head_vars.contains(v));
            }
        }

        // equations in to_simplify have correct indices
        // they are labeled as non-processed
        // their top-most variable is watched
        i = 0;
        for (auto* e : m_to_simplify) {
            VERIFY(e->idx() == i);
            VERIFY(e->state() == to_simplify);
            pdd const& p = e->poly();
            VERIFY(!p.is_val());
            CTRACE("dd.solver", !m_watch.empty() && !m_watch[p.var()].contains(e), 
                   display(tout << "not watched: ", *e) << "\n";);
            VERIFY(m_watch.empty() || m_watch[p.var()].contains(e));            
            ++i;
        }
        // the watch list consists of equations in to_simplify
        // they watch the top most variable in poly
        i = 0;
        for (auto const& w : m_watch) {            
            for (equation* e : w) {
                VERIFY(!e->poly().is_val());
                VERIFY(e->poly().var() == i);
                VERIFY(e->state() == to_simplify);
                VERIFY(m_to_simplify.contains(e));
            }
            ++i;
        }
    }
}
