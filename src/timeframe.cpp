#include "minetrack.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace {

Integer integer(std::string_view text) {
    Integer value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::runtime_error("Invalid interval or UTC time: " + std::string(text));
    return value;
}

std::string padded(Integer value, std::size_t width = 2) {
    std::string text = std::to_string(value);
    if (text.size() < width) text.insert(0, width - text.size(), '0');
    return text;
}

} // namespace

Interval parse_interval(std::string_view value, bool allow_raw) {
    using Unit = Interval::Unit;
    if (allow_raw && value == "raw") return {Unit::raw, 1, "raw"};
    if (value == "day") return {Unit::day, 1, "day"};
    if (value == "week") return {Unit::week, 1, "week"};
    if (value == "month") return {Unit::month, 1, "month"};
    if (value == "year") return {Unit::year, 1, "year"};
    if (value == "hour") value = "1h";
    if (value == "minute") value = "1m";
    if (value == "second") value = "1s";
    const auto suffix_begin = value.find_first_not_of("0123456789");
    if (suffix_begin == 0 || suffix_begin == std::string_view::npos)
        throw std::runtime_error("Use day, week, month, year or a positive interval such as 15m, 4h, 2d, 3mo");
    Integer count = integer(value.substr(0, suffix_begin));
    const auto suffix = value.substr(suffix_begin);
    Unit unit;
    std::string canonical;
    if (suffix == "s") { unit = Unit::second; canonical = "s"; }
    else if (suffix == "m") { unit = Unit::minute; canonical = "m"; }
    else if (suffix == "h") { unit = Unit::hour; canonical = "h"; }
    else if (suffix == "d") { unit = Unit::day; canonical = "d"; }
    else if (suffix == "w") { unit = Unit::week; canonical = "w"; }
    else if (suffix == "mo" || suffix == "M") { unit = Unit::month; canonical = "mo"; }
    else if (suffix == "y") { unit = Unit::year; canonical = "y"; }
    else throw std::runtime_error("Unknown interval unit: " + std::string(suffix));
    if (count < 1) throw std::runtime_error("Interval count must be positive");
    if (unit == Unit::month || unit == Unit::year) {
        const Integer maximum = unit == Unit::month ? 9999 * 12 : 9999;
        if (count > maximum) throw std::runtime_error("Interval exceeds the supported calendar range");
    } else {
        const Interval single{unit, 1, canonical};
        const Integer maximum = (last_supported_day.time_since_epoch().count() + 1) * day_ms;
        if (count > maximum / single.milliseconds())
            throw std::runtime_error("Interval exceeds the supported date range");
        if (unit != Unit::week) {
            const Integer width = single.milliseconds() * count;
            if (width % day_ms == 0) {
                unit = Unit::day; count = width / day_ms; canonical = "d";
            } else if (width % 3'600'000 == 0) {
                unit = Unit::hour; count = width / 3'600'000; canonical = "h";
            } else if (width % 60'000 == 0) {
                unit = Unit::minute; count = width / 60'000; canonical = "m";
            }
        }
    }
    std::string spec = std::to_string(count) + canonical;
    if (count == 1) {
        if (unit == Unit::day) spec = "day";
        if (unit == Unit::week) spec = "week";
        if (unit == Unit::month) spec = "month";
        if (unit == Unit::year) spec = "year";
    }
    return {unit, count, spec};
}

Integer Interval::milliseconds() const {
    switch (unit) {
        case Unit::raw: return 0;
        case Unit::second: return count * 1000;
        case Unit::minute: return count * 60'000;
        case Unit::hour: return count * 3'600'000;
        case Unit::day: return count * day_ms;
        case Unit::week: return count * 7 * day_ms;
        default: throw std::runtime_error("Calendar intervals have variable duration");
    }
}

std::pair<Integer, Integer> Interval::bounds(Integer timestamp) const {
    using namespace std::chrono;
    if (unit == Unit::raw) return {timestamp, timestamp + 1};
    if (unit == Unit::month || unit == Unit::year) {
        const year_month_day date{Day{days{timestamp / day_ms}}};
        const Integer index = unit == Unit::month
            ? (int(date.year()) - 1970) * 12 + unsigned(date.month()) - 1
            : int(date.year()) - 1970;
        const Integer begin = index / count * count;
        const auto boundary = [&](Integer offset) {
            const Integer y = 1970 + (unit == Unit::month ? offset / 12 : offset);
            const unsigned m = unit == Unit::month ? static_cast<unsigned>(offset % 12 + 1) : 1;
            return Day{year{static_cast<int>(y)} / m / 1}.time_since_epoch().count() * day_ms;
        };
        return {boundary(begin), boundary(begin + count)};
    }
    const Integer width = milliseconds();
    // Monday 1969-12-29 anchors UTC weeks.
    const Integer offset = unit == Unit::week ? 3 * day_ms : 0;
    const Integer begin = (timestamp + offset) / width * width - offset;
    return {begin, begin + width};
}

std::string Interval::label(Integer start) const {
    const std::string time = iso_time(start);
    if (unit == Unit::year) return time.substr(0, 4);
    if (unit == Unit::month) return time.substr(0, 7);
    if (unit == Unit::day || unit == Unit::week) return time.substr(0, 10);
    return time.substr(0, unit == Unit::second ? 19 : 16);
}

std::string Interval::chinese() const {
    switch (unit) {
        case Unit::raw: return "原始采样";
        case Unit::second: return std::to_string(count) + "秒";
        case Unit::minute: return std::to_string(count) + "分钟";
        case Unit::hour: return std::to_string(count) + "小时";
        case Unit::day: return std::to_string(count) + "日";
        case Unit::week: return std::to_string(count) + "周";
        case Unit::month: return std::to_string(count) + "月";
        case Unit::year: return std::to_string(count) + "年";
    }
    return {};
}

Interval default_base(const Interval& period) {
    using Unit = Interval::Unit;
    if (period.unit == Unit::month || period.unit == Unit::year || period.unit == Unit::week)
        return parse_interval("day");
    const Integer width = period.milliseconds();
    if (width < 60'000) return parse_interval("raw", true);
    const Integer preferred = width >= 7 * day_ms ? day_ms :
        width >= day_ms ? 3'600'000 : width >= 3'600'000 ? 60'000 : 10'000;
    const Integer base = std::gcd(width, preferred);
    if (base == day_ms) return parse_interval("day");
    if (base % 3'600'000 == 0) return parse_interval(std::to_string(base / 3'600'000) + "h");
    if (base % 60'000 == 0) return parse_interval(std::to_string(base / 60'000) + "m");
    return parse_interval(std::to_string(base / 1000) + "s");
}

void validate_base(const Interval& period, const Interval& base) {
    using Unit = Interval::Unit;
    if (base.unit == Unit::raw) return;
    if (base.unit == Unit::month || base.unit == Unit::year || base.unit == Unit::week ||
        base.milliseconds() > day_ms)
        throw std::runtime_error("--base must be raw or a fixed interval of at most one day");
    const Integer period_width = period.unit == Unit::month || period.unit == Unit::year
        ? day_ms : period.milliseconds();
    if (period_width % base.milliseconds() != 0)
        throw std::runtime_error("--base must divide the candle duration (and the UTC day for calendar candles)");
    if (period.unit == Unit::week && (3 * day_ms) % base.milliseconds() != 0)
        throw std::runtime_error("--base must align with the UTC Monday week boundary");
}

Integer parse_time_bound(std::string_view value, bool end) {
    if (value.ends_with('Z')) value.remove_suffix(1);
    const Day date = parse_day(value.substr(0, 10));
    const Integer start = date.time_since_epoch().count() * day_ms;
    if (value.size() == 10) return start + (end ? day_ms : 0);
    if ((value.size() != 16 && value.size() != 19 && (value.size() < 21 || value.size() > 23)) ||
        (value[10] != 'T' && value[10] != ' ') || value[13] != ':' ||
        (value.size() >= 19 && value[16] != ':') || (value.size() > 19 && value[19] != '.'))
        throw std::runtime_error("Expected a UTC date or YYYY-MM-DDTHH:MM[:SS[.sss]]Z");
    const Integer hour = integer(value.substr(11, 2));
    const Integer minute = integer(value.substr(14, 2));
    const Integer second = value.size() >= 19 ? integer(value.substr(17, 2)) : 0;
    Integer millis = 0;
    if (value.size() > 19) {
        millis = integer(value.substr(20));
        for (std::size_t size = value.size() - 20; size < 3; ++size) millis *= 10;
    }
    if (hour > 23 || minute > 59 || second > 59 || hour < 0 || minute < 0 || second < 0 || millis < 0)
        throw std::runtime_error("Invalid UTC clock time");
    return start + hour * 3'600'000 + minute * 60'000 + second * 1000 + millis;
}

std::string iso_time(Integer timestamp) {
    using namespace std::chrono;
    const auto instant = sys_time<milliseconds>{milliseconds{timestamp}};
    const Day day = floor<days>(instant);
    const Integer remainder = (instant - day).count();
    const std::string fraction = remainder % 1000 == 0 ? "" : "." + padded(remainder % 1000, 3);
    return iso_day(day) + "T" + padded(remainder / 3'600'000) + ":" +
        padded(remainder / 60'000 % 60) + ":" + padded(remainder / 1000 % 60) + fraction + "Z";
}
