// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include "rdb_protocol/error.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/query.hpp"
#include "math.hpp"

namespace ql {

class var_term_t : public term_t {
public:
    var_term_t(compile_env_t *env, const raw_term_t *term)
            : term_t(term) {
        rcheck(term->num_args() == 1, base_exc_t::GENERIC,
               "A variable term has the wrong number of arguments.");

        const raw_term_t *arg0 = term->args().next();
        rcheck(arg0->type == Term::DATUM, base_exc_t::GENERIC,
               "A variable term has a non-numeric argument.");
        rcheck(arg0->value.get_type() == datum_t::type_t::R_NUM, base_exc_t::GENERIC,
               "A variable term has a non-numeric variable name argument.");

        varname = sym_t(arg0->value.as_int());
        rcheck(env->visibility.contains_var(varname), base_exc_t::GENERIC,
               "Variable name not found.");
    }

private:
    virtual void accumulate_captures(var_captures_t *captures) const {
        captures->vars_captured.insert(varname);
    }

    virtual bool is_deterministic() const {
        return true;
    }

    sym_t varname;
    virtual scoped_ptr_t<val_t> term_eval(scope_env_t *env, eval_flags_t) const {
        return new_val(env->scope.lookup_var(varname));
    }
    virtual const char *name() const { return "var"; }
};

class implicit_var_term_t : public term_t {
public:
    implicit_var_term_t(compile_env_t *env, const raw_term_t *term)
        : term_t(term) {
        rcheck(
            term->num_args() == 0 && term->num_optargs() == 0, base_exc_t::GENERIC,
            "Expected no arguments or optional arguments on implicit variable term.");

        rcheck(env->visibility.implicit_is_accessible(), base_exc_t::GENERIC,
               env->visibility.get_implicit_depth() == 0
               ? "r.row is not defined in this context."
               : "Cannot use r.row in nested queries.  Use functions instead.");
    }
private:
    virtual void accumulate_captures(var_captures_t *captures) const {
        captures->implicit_is_captured = true;
    }

    virtual bool is_deterministic() const {
        return true;
    }

    virtual scoped_ptr_t<val_t> term_eval(scope_env_t *env, UNUSED eval_flags_t flags) const {
        return new_val(env->scope.lookup_implicit());
    }
    virtual const char *name() const { return "implicit_var"; }
};

counted_t<term_t> make_var_term(
        compile_env_t *env, const raw_term_t *term) {
    return make_counted<var_term_t>(env, term);
}
counted_t<term_t> make_implicit_var_term(
        compile_env_t *env, const raw_term_t *term) {
    return make_counted<implicit_var_term_t>(env, term);
}

} // namespace ql
