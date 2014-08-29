// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/term.hpp"
#include "rdb_protocol/terms/terms.hpp"

namespace ql {
class json_term_t : public op_term_t {
public:
    json_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1)) { }

    counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        const datum_string_t &data = args->arg(env, 0)->as_str();
        // TODO (daniel): This causes copying, which might reduce performance and
        // wastes memory. Change the cJSON interface to take a length.
        // Or maybe store datum_string_ts with a null byte appended, though that
        // would suck.
        const std::string std_data = data.to_std();
        scoped_cJSON_t cjson(cJSON_Parse(std_data.c_str()));
        rcheck(cjson.get() != NULL, base_exc_t::GENERIC,
               strprintf("Failed to parse \"%s\" as JSON.",
                 (data.size() > 40
                  ? (std_data.substr(0, 37) + "...").c_str()
                  : std_data.c_str())));
        return new_val(to_datum(cjson.get(), env->env->limits()));
    }

    virtual const char *name() const { return "json"; }
};

counted_t<term_t> make_json_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<json_term_t>(env, term);
}
} // namespace ql
