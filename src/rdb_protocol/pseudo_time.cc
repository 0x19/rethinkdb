// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/pseudo_time.hpp"

#include <time.h>
#include <math.h>

#include "errors.hpp"
#include <boost/date_time.hpp>

#include "rdb_protocol/datum.hpp"
#include "utils.hpp"

namespace ql {
namespace pseudo {

const char *const time_string = "TIME";
const char *const epoch_time_key = "epoch_time";
const char *const timezone_key = "timezone";

typedef boost::local_time::local_time_input_facet input_timefmt_t;
typedef boost::local_time::local_time_facet output_timefmt_t;
typedef boost::local_time::local_date_time time_t;
typedef boost::posix_time::ptime ptime_t;
typedef boost::posix_time::time_duration dur_t;
typedef boost::gregorian::date date_t;

// Some notes on our ISO 8601 parsing --
// * We use %ZP instead of %Q because I can't get %Q to work.  I don't know why,
//   the documentation says it should, but there you go.
// * The above is fine because we need a sanitization step anyway.
// * We need a sanitization step because Boost is very...liberal in its parsing,
//   so much so that it will sometimes parse total gibberish.  When I tried
//   the classic approach of simply listing all the possible formats (you can
//   see the list in 06f4535 or earlier), I ended up needing about 90 formats,
//   and with that many options to work with boost would parse basically
//   anything.  It would also often parse correct dates in one format
//   (especially week dates; see below) as incorrect dates in a similar format,
//   happily skipping characters and leaving garbage at the end in pursuit of a
//   valid interpretation of the input string.
// * We can't support week dates right now, because Boost doesn't allow %V or %u
//   in parsing right now.  This will theoretically change at some point in the
//   future.
// * I hate boost, I hate dates, and most of all, I hate myself.
const std::locale daycount_format =
    std::locale(std::locale::classic(), new input_timefmt_t("%Y-%jT%H:%M:%s%ZP"));
// One day...
// const std::locale weekcount_format =
//     std::locale(std::locale::classic(), new input_timefmt_t("%Y-W%V-%uT%H:%M:%s%ZP"));
const std::locale month_day_format =
    std::locale(std::locale::classic(), new input_timefmt_t("%Y-%m-%dT%H:%M:%s%ZP"));

const ptime_t raw_epoch(date_t(1970, 1, 1));
const boost::local_time::time_zone_ptr utc(
    new boost::local_time::posix_time_zone("UTC"));
const boost::local_time::local_date_time epoch(raw_epoch, utc);

// Boost's documentation on what errors may be thrown where is somewhat lacking,
// so this is probably a slight superset of what we actually need to handle.  We
// may also need to catch the following exceptions in the future, but I omitted
// them because we never seem to encounter them and they might catch legitimate
// non-boost exceptions as well:
// * std::out_of_range
// * std::invalid_argument
// * std::runtime_error
// (Note: we catch `std::ios_base::failure` because Boost uses that to report
// parsing errors, and we don't use streams anywhere else.)
#define HANDLE_BOOST_ERRORS(TARGET)                                     \
      catch (const boost::gregorian::bad_year &e) {                     \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::gregorian::bad_month &e) {                    \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::gregorian::bad_day_of_month &e) {             \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::gregorian::bad_weekday &e) {                  \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::local_time::bad_offset &e) {                  \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::local_time::bad_adjustment &e) {              \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::local_time::time_label_invalid &e) {          \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::local_time::dst_not_valid &e) {               \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::local_time::ambiguous_result &e) {            \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::local_time::data_not_accessible &e) {         \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const boost::local_time::bad_field_count &e) {             \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    } catch (const std::ios_base::failure &e) {                         \
        rfail_target(TARGET, base_exc_t::GENERIC,                       \
                     "Error in time logic: %s.", e.what());             \
    }                                                                   \

// Produces a datum_exc_t instead
static const datum_t dummy_datum(nullptr);
#define HANDLE_BOOST_ERRORS_NO_TARGET HANDLE_BOOST_ERRORS(&dummy_datum)

enum date_format_t { UNSET, MONTH_DAY, WEEKCOUNT, DAYCOUNT };

// This is where we do our sanitization.
namespace sanitize {

// Copy n digits from `s` to the end of `*p_out`, starting at `*p_at`.
// Increment `*p_at` by the number of digits copied.  Throw on any error.
void mandatory_digits(const std::string &s, size_t n, size_t *p_at, std::string *p_out) {
    for (size_t i = 0; i < n; ++i) {
        size_t at = (*p_at)++;
        rcheck_datum(at < s.size(), base_exc_t::GENERIC,
                     strprintf("Truncated date string `%s`.", s.c_str()));
        char c = s[at];
        rcheck_datum('0' <= c && c <= '9', base_exc_t::GENERIC,
                     strprintf(
                         "Invalid date string `%s` (got `%c` but expected a digit).",
                         s.c_str(), c));
        if (p_out) {
            *p_out += c;
        }
    }
}

enum optional_char_default_behavior_t { INCLUDE, EXCLUDE };
// If `s[*p_at]` is `c`, increment `*p_at` and add `c` to the end of `*p_out`.
// Otherwise, if `default_behavior` is `INCLUDE`, add `c` to the end of `*p_out`
// anyway.  Return whether or not `*p_at` was incremented.
bool optional_char(const std::string &s, char c, size_t *p_at, std::string *p_out,
                   optional_char_default_behavior_t default_behavior = INCLUDE) {
    bool consumed = false;
    size_t at = *p_at;
    if (at < s.size() && s[at] == c) {
        (*p_at) += 1;
        consumed = true;
        *p_out += c;
    } else {
        if (default_behavior == INCLUDE) {
            *p_out += c;
        }
    }
    return consumed;
}

// Sanitize a date, and return which format it's in.
std::string date(const std::string &s, date_format_t *df_out) {
    std::string out;
    size_t at = 0;
    // Add Year
    mandatory_digits(s, 4, &at, &out);
    if (at == s.size()) {
        *df_out = MONTH_DAY;
        return out + "-01-01";
    }
    // We need to keep track of this because YYYY-MM and YYYYMMDD are valid, but
    // YYYYMM is not.  I don't write these standards.
    bool first_hyphen = optional_char(s, '-', &at, &out);
    if (optional_char(s, 'W', &at, &out, EXCLUDE)) {
        *df_out = WEEKCOUNT;
        mandatory_digits(s, 2, &at, &out);
        if (at == s.size()) {
            return out + "-1"; // 1 through 7
        }
        optional_char(s, '-', &at, &out);
        mandatory_digits(s, 1, &at, &out);
    } else if (s.size() - at == 3) {
        *df_out = DAYCOUNT;
        mandatory_digits(s, 3, &at, &out);
    } else {
        *df_out = MONTH_DAY;
        mandatory_digits(s, 2, &at, &out);
        if (first_hyphen && at == s.size()) {
            return out + "-01";
        }
        bool second_hyphen = optional_char(s, '-', &at, &out);
        rcheck_datum(!(first_hyphen ^ second_hyphen), base_exc_t::GENERIC,
                     strprintf("Date string `%s` must have 0 or 2 hyphens.", s.c_str()));
        mandatory_digits(s, 2, &at, &out);
    }
    rcheck_datum(at == s.size(), base_exc_t::GENERIC,
                 strprintf("Garbage characters `%s` at end of date string `%s`.",
                           s.substr(at).c_str(), s.c_str()));
    return out;
}

// Sanitize a time.
std::string time(const std::string &s) {
    std::string out;
    size_t at = 0;
    mandatory_digits(s, 2, &at, &out);
    if (at == s.size()) {
        return out + ":00:00.000";
    }
    bool first_colon = optional_char(s, ':', &at, &out);
    mandatory_digits(s, 2, &at, &out);
    if (at == s.size()) {
        return out + ":00.000";
    }
    bool second_colon = optional_char(s, ':', &at, &out);
    rcheck_datum(!(first_colon ^ second_colon), base_exc_t::GENERIC,
                 strprintf("Time string `%s` must have 0 or 2 colons.", s.c_str()));
    mandatory_digits(s, 2, &at, &out);
    if (optional_char(s, '.', &at, &out)) {
        size_t read = 0;
        while (at < s.size() && read < 3) {
            mandatory_digits(s, 1, &at, &out);
            read += 1;
        }
        while (at < s.size()) {
            mandatory_digits(s, 1, &at, NULL);
        }
        while (read++ < 3) {
            out += '0';
        }
    } else {
        out += "000";
    }
    rcheck_datum(at == s.size(), base_exc_t::GENERIC,
                 strprintf("Garbage characters `%s` at end of time string `%s`.",
                           s.substr(at).c_str(), s.c_str()));
    return out;
}

bool hours_valid(char l, char r) {
    return ((l == '0' || l == '1') && ('0' <= r && r <= '9'))
        || ((l == '2') && ('0' <= r && r <= '4'));
}
bool minutes_valid(char l, char r) {
    return ('0' <= l && l <= '5') && ('0' <= r && r <= '9');
}

// Sanitize a timezone.
std::string tz(const std::string &s) {
    rcheck_datum(s != "-00" && s != "-00:00", base_exc_t::GENERIC,
                 strprintf("`%s` is not a valid time offset.", s.c_str()));
    if (s == "Z") {
        return "+00:00";
    }
    std::string out;
    size_t at = 0;
    bool sign_prefix = optional_char(s, '-', &at, &out, EXCLUDE)
        || optional_char(s, '+', &at, &out, EXCLUDE);
    rcheck_datum(sign_prefix, base_exc_t::GENERIC,
                 strprintf("Timezone `%s` does not start with `-` or `+`.", s.c_str()));
    mandatory_digits(s, 2, &at, &out);
    if (at == s.size()) {
        return out + ":00";
    }
    optional_char(s, ':', &at, &out);
    mandatory_digits(s, 2, &at, &out);
    rcheck_datum(at == s.size(), base_exc_t::GENERIC,
                 strprintf("Garbage characters `%s` at end of timezone string `%s`.",
                           s.substr(at).c_str(), s.c_str()));

    r_sanity_check(out.size() == 6);
    rcheck_datum(hours_valid(out[1], out[2]), base_exc_t::GENERIC,
                 strprintf("Hours out of range in `%s`.", s.c_str()));
    rcheck_datum(minutes_valid(out[4], out[5]), base_exc_t::GENERIC,
                 strprintf("Minutes out of range in `%s`.", s.c_str()));
    return out;
}

// Sanitize an ISO 8601 string.
std::string iso8601(const std::string &s, const std::string &default_tz, date_format_t *df_out) {
    std::string date_s, time_s, tz_s;
    size_t tloc, start, sign_loc;
    tloc = s.find('T');
    date_s = date(s.substr(0, tloc), df_out);
    if (tloc == std::string::npos) {
        time_s = "00:00:00.000";
        tz_s = default_tz;
    } else {
        start = tloc + 1;
        sign_loc = s.find('-', start);
        sign_loc = (sign_loc == std::string::npos) ? s.find('+', start) : sign_loc;
        sign_loc = (sign_loc == std::string::npos) ? s.find('Z', start) : sign_loc;
        time_s = time(s.substr(start, sign_loc - start));
        if (sign_loc == std::string::npos) {
            tz_s = default_tz;
        } else {
            tz_s = tz(s.substr(sign_loc));
        }
    }
    return date_s + "T" + time_s + tz_s;
}

} // namespace sanitize

bool tz_valid(const std::string &tz, std::string *tz_out = NULL) {
    try {
        std::string s = sanitize::tz(tz);
        if (tz_out) {
            *tz_out = s;
        }
    } catch (const datum_exc_t &e) {
        return false;
    }
    return true;
}

// Sanitize the timezone we retrieve from a boost local time.  Boost local time
// gives a slight superset of ISO 8601 even when only fed ISO 8601 timezones, so
// we adjust for that here.
std::string sanitize_boost_tz(std::string tz, const rcheckable_t *target) {
    size_t colpos = tz.find(':');
    if (colpos != std::string::npos && (colpos + 1) < tz.size() && tz[colpos+1] == '-') {
        tz = tz.substr(0, colpos + 1) + tz.substr(colpos + 2, std::string::npos);
    }
    rcheck_target(target,  base_exc_t::GENERIC, tz != "UTC+00" && tz != "",
                  "ISO 8601 string has no time zone, and no default time "
                  "zone was provided.");

    std::string tz_out;
    if (tz == "Z+00") {
        return "+00:00";
    } else if (tz_valid(tz, &tz_out)) {
        return tz_out;
    }
    rfail_target(target, base_exc_t::GENERIC,
                 "Invalid ISO 8601 timezone: `%s`.", tz.c_str());
}

counted_t<const datum_t> boost_to_time(time_t t, const rcheckable_t *target) {
    dur_t dur(t - epoch);
    double seconds = dur.total_microseconds() / 1000000.0;
    std::string tz = t.zone_as_posix_string();
    tz = sanitize_boost_tz(tz, target);
    r_sanity_check(tz_valid(tz));
    return make_time(seconds, tz);
}

counted_t<const datum_t> iso8601_to_time(
    const std::string &s, const std::string &default_tz, const rcheckable_t *target) {
    try {
        date_format_t df = UNSET;
        std::string sanitized;
        try {
            sanitized = sanitize::iso8601(s, default_tz, &df);
        } catch (const datum_exc_t &e) {
            rfail_target(target, base_exc_t::GENERIC, "%s", e.what());
        }

        std::istringstream ss(sanitized);
        ss.exceptions(std::ios_base::failbit);
        switch (df) {
        case UNSET: r_sanity_check(false); break;
        case MONTH_DAY: ss.imbue(month_day_format); break;
        case WEEKCOUNT: {
            rfail_target(target, base_exc_t::GENERIC, "%s",
                         "Due to limitations in the boost time library we use for "
                         "parsing, we cannot support ISO week dates right now.  "
                         "Sorry about that!  Please use years, calendar dates, or "
                         "ordinal dates instead.");
        } break;
        case DAYCOUNT: ss.imbue(daycount_format); break;
        default: unreachable();
        }
        time_t t(boost::date_time::not_a_date_time);
        ss >> t;
        rcheck_target(
            target, base_exc_t::GENERIC, !t.is_special(),
            strprintf("Failed to parse `%s` (`%s`) as ISO 8601 time.",
                      s.c_str(), sanitized.c_str()));
        return boost_to_time(t, target);
    } HANDLE_BOOST_ERRORS(target);
}

const int64_t sec_incr = INT_MAX;
void add_seconds_to_ptime(ptime_t *t, double raw_sec) {
    int64_t sec = raw_sec;
    int64_t microsec = (raw_sec * 1000000.0) - (sec * 1000000);

    // boost::posix_time::seconds doesn't like large numbers, and like any
    // mature library, it reacts by silently overflowing somewhere and producing
    // an incorrect date if you give it a number that it doesn't like.
    int sign = sec < 0 ? -1 : 1;
    sec *= sign;
    while (sec > 0) {
        int64_t diff = std::min(sec, sec_incr);
        sec -= diff;
        *t += boost::posix_time::seconds(diff * sign);
    }
    r_sanity_check(sec == 0);

    *t += boost::posix_time::microseconds(microsec);
}

time_t time_to_boost(counted_t<const datum_t> d) {
    double raw_sec = d->get(epoch_time_key)->as_num();
    ptime_t t(date_t(1970, 1, 1));
    add_seconds_to_ptime(&t, raw_sec);

    if (counted_t<const datum_t> tz = d->get(timezone_key, NOTHROW)) {
        boost::local_time::time_zone_ptr zone(
            new boost::local_time::posix_time_zone(sanitize::tz(tz->as_str().to_std())));
        return time_t(t, zone);
    } else {
        return time_t(t, utc);
    }
}

const std::locale tz_format =
    std::locale(std::locale::classic(), new output_timefmt_t("%Y-%m-%dT%H:%M:%S%F%Q"));
const std::locale no_tz_format =
    std::locale(std::locale::classic(), new output_timefmt_t("%Y-%m-%dT%H:%M:%S%F"));
std::string time_to_iso8601(counted_t<const datum_t> d) {
    try {
        time_t t = time_to_boost(d);
        int year = t.date().year();
        // Boost also accepts year 10000.  I don't think any real users will hit
        // that edge case, but better safe than sorry.
        rcheck_datum(year >= 0 && year <= 9999, base_exc_t::GENERIC,
                     strprintf("Year `%d` out of valid ISO 8601 range [0, 9999].",
                               year));
        std::ostringstream ss;
        ss.exceptions(std::ios_base::failbit);
        if (counted_t<const datum_t> tz = d->get(timezone_key, NOTHROW)) {
            ss.imbue(tz_format);
        } else {
            ss.imbue(no_tz_format);
        }
        ss << time_to_boost(d);
        std::string s = ss.str();
        size_t dot_off = s.find('.');
        return (dot_off == std::string::npos) ? s :
            s.substr(0, dot_off + 4) + s.substr(dot_off + 7, std::string::npos);
    } HANDLE_BOOST_ERRORS_NO_TARGET;
}

double time_to_epoch_time(counted_t<const datum_t> d) {
    return d->get(epoch_time_key)->as_num();
}

counted_t<const datum_t> time_now() {
    try {
        ptime_t t = boost::posix_time::microsec_clock::universal_time();
        return make_time((t - raw_epoch).total_microseconds() / 1000000.0, "+00:00");
    } HANDLE_BOOST_ERRORS_NO_TARGET;
}

int time_cmp(const datum_t &x, const datum_t &y) {
    r_sanity_check(x.is_ptype(time_string));
    r_sanity_check(y.is_ptype(time_string));
    return x.get(epoch_time_key)->cmp(*y.get(epoch_time_key));
}

double sanitize_epoch_sec(double d) {
    return round(d * 1000) / 1000;
}

void sanitize_time(datum_t *time) {
    r_sanity_check(time != NULL);
    r_sanity_check(time->is_ptype(time_string));
    std::string msg;
    bool has_epoch_time = false;
    bool has_timezone = false;
    for (auto it = time->as_object().begin(); it != time->as_object().end(); ++it) {
        if (it->first == epoch_time_key) {
            if (it->second->get_type() == datum_t::R_NUM) {
                has_epoch_time = true;
                double d = it->second->as_num();
                double d2 = sanitize_epoch_sec(d);
                if (d2 != d) {
                    bool b = time->add(epoch_time_key,
                                       make_counted<const datum_t>(d2),
                                       CLOBBER);
                    r_sanity_check(b);
                }
            } else {
                msg = strprintf("field `%s` must be a number (got `%s` of type %s)",
                                epoch_time_key, it->second->trunc_print().c_str(),
                                it->second->get_type_name().c_str());
                break;
            }
        } else if (it->first == timezone_key) {
            if (it->second->get_type() == datum_t::R_STR) {
                const std::string raw_tz = it->second->as_str().to_std();
                std::string tz;
                if (tz_valid(raw_tz, &tz)) {
                    has_timezone = true;
                    tz = (tz == "Z") ? "+00:00" : tz;
                    if (tz != raw_tz) {
                        bool b = time->add(timezone_key,
                                           make_counted<const datum_t>(std::move(tz)),
                                           CLOBBER);
                        r_sanity_check(b);
                    }
                    continue;
                } else {
                    msg = strprintf("invalid timezone string `%s`",
                                    it->second->trunc_print().c_str());
                    break;
                }
            } else {
                msg = strprintf("field `%s` must be a string (got `%s` of type %s)",
                                timezone_key, it->second->trunc_print().c_str(),
                                it->second->get_type_name().c_str());
                break;
            }
        } else if (it->first == datum_t::reql_type_string) {
            continue;
        } else {
            msg = strprintf("unrecognized field `%s`", it->first.c_str());
            break;
        }
    }

    if (msg == "" && !has_epoch_time) {
        msg = strprintf("no field `%s`", epoch_time_key);
    } else if (msg == "" && !has_timezone) {
        msg = strprintf("no field `%s`", timezone_key);
    }

    if (msg != "") {
        rfail_target(time, base_exc_t::GENERIC,
                     "Invalid time object constructed (%s):\n%s",
                     msg.c_str(), time->trunc_print().c_str());
    }
}

counted_t<const datum_t> time_tz(counted_t<const datum_t> time) {
    r_sanity_check(time->is_ptype(time_string));
    if (counted_t<const datum_t> tz = time->get(timezone_key, NOTHROW)) {
        return tz;
    } else {
        return make_counted<const datum_t>(nullptr);
    }
}

counted_t<const datum_t> time_in_tz(counted_t<const datum_t> t,
                                    counted_t<const datum_t> tz) {
    r_sanity_check(t->is_ptype(time_string));
    datum_ptr_t t2(t->as_object());
    std::string raw_new_tzs = tz->as_str().to_std();
    std::string new_tzs = sanitize::tz(raw_new_tzs);
    if (raw_new_tzs == new_tzs) {
        UNUSED bool b = t2.add(timezone_key, tz, CLOBBER);
    } else {
        UNUSED bool b =
            t2.add(timezone_key, make_counted<const datum_t>(std::move(new_tzs)), CLOBBER);
    }
    return t2.to_counted();
}

counted_t<const datum_t> make_time(double epoch_time, std::string tz) {
    std::map<std::string, counted_t<const datum_t> > map
        = { { datum_t::reql_type_string, make_counted<const datum_t>(time_string) },
            { epoch_time_key, make_counted<const datum_t>(epoch_time) },
            { timezone_key, make_counted<const datum_t>(std::move(tz)) } };
    return make_counted<datum_t>(std::move(map));
}

counted_t<const datum_t> make_time(
    int year, int month, int day, int hours, int minutes, double seconds,
    std::string tz, const rcheckable_t *target) {
    try {
        ptime_t ptime(date_t(year, month, day), dur_t(hours, minutes, 0));
        add_seconds_to_ptime(&ptime, seconds);
        try {
            tz = sanitize::tz(tz);
        } catch (const datum_exc_t &e) {
            rfail_target(target, base_exc_t::GENERIC, "%s", e.what());
        }
        boost::local_time::time_zone_ptr zone(
            new boost::local_time::posix_time_zone(tz));
        return boost_to_time(time_t(ptime, zone) - zone->base_utc_offset(), target);
    } HANDLE_BOOST_ERRORS(target);
}

counted_t<const datum_t> time_add(counted_t<const datum_t> x,
                                  counted_t<const datum_t> y) {
    counted_t<const datum_t> time, duration;
    if (x->is_ptype(time_string)) {
        time = x;
        duration = y;
    } else {
        r_sanity_check(y->is_ptype(time_string));
        time = y;
        duration = x;
    }

    datum_object_builder_t res(time->as_object());
    res.overwrite(
        epoch_time_key,
        make_counted<datum_t>(time->get(epoch_time_key)->as_num() +
                              duration->as_num()));

    return std::move(res).to_counted();
}

counted_t<const datum_t> time_sub(counted_t<const datum_t> time,
                                  counted_t<const datum_t> time_or_duration) {
    r_sanity_check(time->is_ptype(time_string));

    if (time_or_duration->is_ptype(time_string)) {
        return make_counted<const datum_t>(sanitize_epoch_sec(
            time->get(epoch_time_key)->as_num()
            - time_or_duration->get(epoch_time_key)->as_num()));
    } else {
        datum_object_builder_t res(time->as_object());
        res.overwrite(
            epoch_time_key,
            make_counted<const datum_t>(time->get(epoch_time_key)->as_num() -
                                        time_or_duration->as_num()));
        return std::move(res).to_counted();
    }
}

double time_portion(counted_t<const datum_t> time, time_component_t c) {
    try {
        ptime_t ptime = time_to_boost(time).local_time();
        switch (c) {
        case YEAR: return ptime.date().year();
        case MONTH: return ptime.date().month();
        case DAY: return ptime.date().day();
        case DAY_OF_WEEK: {
            // We use the ISO 8601 convention which counts from 1 and starts with Monday.
            int d = ptime.date().day_of_week();
            return d == 0 ? 7 : d;
        } break;
        case DAY_OF_YEAR: return ptime.date().day_of_year();
        case HOURS: return ptime.time_of_day().hours();
        case MINUTES: return ptime.time_of_day().minutes();
        case SECONDS: {
            double frac = modf(time->get(epoch_time_key)->as_num(), &frac);
            frac = round(frac * 1000) / 1000;
            return ptime.time_of_day().seconds() + frac;
        } break;
        default: unreachable();
        }
    } HANDLE_BOOST_ERRORS_NO_TARGET;
}

time_t boost_date(time_t boost_time) {
    ptime_t ptime = boost_time.local_time();
    date_t d(ptime.date().year_month_day());
    return time_t(ptime_t(d), boost_time.zone());
}

counted_t<const datum_t> time_date(counted_t<const datum_t> time,
                                   const rcheckable_t *target) {
    try {
        return boost_to_time(boost_date(time_to_boost(time)), target);
    } HANDLE_BOOST_ERRORS(target);
}

counted_t<const datum_t> time_of_day(counted_t<const datum_t> time) {
    try {
        time_t boost_time = time_to_boost(time);
        double sec =
            (boost_time - boost_date(boost_time)).total_microseconds() / 1000000.0;
        sec = round(sec * 1000) / 1000;
        return make_counted<const datum_t>(sec);
    } HANDLE_BOOST_ERRORS_NO_TARGET;
}

void time_to_str_key(const datum_t &d, std::string *str_out) {
    // We need to prepend "P" and append a character less than [a-zA-Z] so that
    // different pseudotypes sort correctly.
    str_out->append(std::string("P") + time_string + ":");
    d.get(epoch_time_key)->num_to_str_key(str_out);
}

} // namespace pseudo
} // namespace ql
