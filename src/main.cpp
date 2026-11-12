#include "minetrack.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace std::chrono;

namespace {

constexpr std::array<std::string_view, 12> candle_methods{
    "index", "none", "mean", "sma", "ema", "wma", "rma", "dema", "tema", "hma", "kama", "median"};

Integer number(std::string_view value) {
    Integer parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        throw std::runtime_error("Expected an integer: " + std::string(value));
    return parsed;
}

double real_number(std::string_view value) {
    double parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !std::isfinite(parsed))
        throw std::runtime_error("Expected a finite number: " + std::string(value));
    return parsed;
}

void usage() {
    std::cout << R"(MineTrack Java archive CLI

Usage: minetrack <command> [options]
  sync                 Download and import the official Java archives
  import <file>        Import a local CSV or an extracted MineTrack SQLite file
  trend                Player and ping statistics per host and UTC period
  candles              Multi-timeframe OHLC from positive means or their EMA
  --list-hosts         List all known database hosts; supports --output/--format
  methods              List candle methods and their behavior (no database needed)
  rank                 Rank hosts by average online players
  coverage             List every UTC date in range, including days with no rows
  raw                  Export original samples with ping status
  sources              Show import progress and duplicate counts

Options:
  --db PATH            Database path (default: minetrack.sqlite)
  --from DATE          First UTC date; candles also accepts ISO date-times
  --to DATE            Last date inclusive; candle date-times are exclusive
  --cache PATH         Sync download cache (default: <db>.downloads)
  --jobs N             Concurrent daily downloads, 1-16 (default: 4)
  --keep-downloads     Keep downloaded and extracted files after sync
  --host HOST          Exact hostname filter (required for candles)
  --period PERIOD      Candles: day/week/month/year, Ns/Nm/Nh/Nd/Nw/Nmo/Ny
                       Candles default: month. Trend: day/month/year, default day
  --base PERIOD        Candle averaging interval or raw (default: automatic)
  --limit N            Last N candles; rank: 20; raw: 1000; 0: unlimited
  --method METHOD      index/none/mean/sma/ema/wma/rma/dema/tema/hma/kama/median
                       Or all: export every method as PNG. Default: index
  --span N             Smoothing length: index candles, others base points (7)
  --range-scale X      Smoothed wick width, 0 < X <= 10 (default: 0.5)
  --font PATH          Optional TrueType/OpenType font (default: system font)
  --with-csv           Save matching OHLC CSV beside each PNG
  --chart-set          Export 13 year/month/week/day history and recent windows
  --format FORMAT      table, csv; candles also supports png
  --output PATH        Write the report to a file
  --chart-dir DIR      Auto-name PNG files in DIR (creates missing directories)
  --help               Show this help

Dates are UTC. For sync, dates select archive files. A selected legacy archive
is imported in full. Report dates filter records by their timestamps.
Candles exclude zero, negative and failed samples. Green rises, red falls.
Mean/ema candles use daily means for month/year/week, hourly means for day,
minute means for hour, and 10-second means for minute. --base overrides this.
None uses positive raw samples; --base and --span cannot change this.
All filters warm up from earlier data. A .png path selects PNG rendering.
Index candles use period means, a smoothed close and continuous deviation bands.
Their OHLC values are derived indicator values.
PNG without --output uses automatic names in ./charts; --chart-dir overrides it.
--method all defaults to PNG, with one file per method in the same directory.
)";
}

bool among(std::string_view value, std::initializer_list<std::string_view> allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

void protect_database(const fs::path& database, const fs::path& output) {
    const auto destination = fs::weakly_canonical(output);
    for (const auto* suffix : {"", "-wal", "-shm", "-journal"}) {
        fs::path protected_path = database;
        protected_path += suffix;
        if (destination == fs::weakly_canonical(protected_path) ||
            (fs::exists(destination) && fs::exists(protected_path) &&
             fs::equivalent(destination, protected_path))) {
            throw std::runtime_error("The report output must differ from the database and its sidecars");
        }
    }
}

std::string filename_part(std::string_view value) {
    std::string result;
    for (const unsigned char ch : value) {
        if (result.size() == 32) break;
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        result += safe ? static_cast<char>(ch) : '_';
    }
    // Avoid hidden names and path components made only of dots.
    while (!result.empty() && result.front() == '.') result.erase(result.begin());
    return result.empty() ? "server" : result;
}

std::string filename_time(Integer value) {
    std::string result;
    for (const char ch : iso_time(value))
        if (ch != '-' && ch != ':' && ch != '.') result += ch;
    return result;
}

fs::path automatic_chart_path(const fs::path& directory, std::string_view host,
                              const CandleOptions& options, const std::vector<Candle>& candles, bool with_csv,
                              std::string_view tag) {
    std::string stem = filename_part(host) + '_' + options.period.spec + '_' + options.method;
    if (options.method != "mean" && options.method != "none") stem += "_s" + std::to_string(options.span);
    stem += "_b" + options.base.spec;
    if (!tag.empty()) stem += '_' + std::string(tag);
    stem += '_' + filename_time(candles.front().first) + '-' +
            filename_time(candles.back().last);
    auto path = directory / path_from_utf8(stem + ".png");
    const auto occupied = [&](const fs::path& png) {
        auto csv = png;
        csv.replace_extension(".csv");
        return fs::exists(fs::symlink_status(png)) || (with_csv && fs::exists(fs::symlink_status(csv)));
    };
    for (Integer suffix = 2; occupied(path); ++suffix)
        path = directory / path_from_utf8(stem + '_' + std::to_string(suffix) + ".png");
    return path;
}

int run(const std::vector<std::string>& args) {
    if (args.empty() || among(args.front(), {"--help", "-h", "help"})) {
        usage();
        return 0;
    }
    const std::string& command = args.front();
    if (command == "methods") {
        if (args.size() != 1) throw std::runtime_error("Usage: minetrack methods");
        std::cout << R"(method  behavior                                      span unit
index   Robust log EMA + smooth deviation bands         output candles (default)
none    Positive raw samples, no averaging/filter       not used; base=raw
mean    Base-interval means, no rolling filter          not used
sma     Simple moving average                           base points
ema     Exponential moving average                      base points
wma     Linear weighted average, newest weight largest  base points
rma     Wilder smoothing, alpha=1/span                  base points
dema    Double EMA, less lag, possible overshoot         base points
tema    Triple EMA, less lag, possible overshoot         base points
hma     Hull WMA combination, possible overshoot        base points
kama    Adaptive average, fast=2, slow=30               base points (ER length)
median  Rolling median, suppresses isolated spikes      base points

All opens equal the previous valid close, including gaps.
Filters are causal. New windows use available points; recursive seeds use
the first point. DEMA/TEMA/HMA outputs are floored at 1e-9 for positive OHLC.
Only index synthesizes deviation bands; other methods use filtered extrema.
)";
        return 0;
    }
    const bool report = among(command, {"trend", "rank", "coverage", "raw", "sources", "candles", "--list-hosts"});
    if (!report && !among(command, {"sync", "import"}))
        throw std::runtime_error("Unknown command: " + command);

    std::map<std::string, std::string, std::less<>> options;
    std::vector<std::string> inputs;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& option = args[i];
        if (option == "--help" || option == "-h") {
            usage();
            return 0;
        }
        if (option == "--") {
            inputs.insert(inputs.end(), args.begin() + i + 1, args.end());
            break;
        }
        if (!option.starts_with('-')) {
            inputs.push_back(option);
            continue;
        }
        const bool range = command == "sync" || (report && command != "sources" && command != "--list-hosts");
        const bool valid = option == "--db" ||
            (range && among(option, {"--from", "--to"})) ||
            (command == "sync" && among(option, {"--cache", "--jobs", "--keep-downloads"})) ||
            (range && report && option == "--host") ||
            (report && among(option, {"--format", "--output"})) ||
            (among(command, {"trend", "candles"}) && option == "--period") ||
            (command == "candles" && among(option, {"--method", "--span", "--base", "--range-scale", "--font", "--with-csv", "--chart-dir", "--chart-set"})) ||
            (among(command, {"rank", "raw", "candles"}) && option == "--limit");
        if (!valid) throw std::runtime_error("Unknown option for " + command + ": " + option);
        if (options.contains(option)) throw std::runtime_error("Repeated option: " + option);
        if (option == "--keep-downloads" || option == "--with-csv" || option == "--chart-set") {
            options.emplace(option, "");
        } else {
            if (i + 1 == args.size() || args[i + 1].empty() || args[i + 1].starts_with("--"))
                throw std::runtime_error("Missing value for " + option);
            options.emplace(option, args[++i]);
        }
    }
    if (inputs.size() != (command == "import" ? 1u : 0u))
        throw std::runtime_error(command == "import" ? "Provide one input file" : "Unexpected positional argument");

    const auto option = [&](std::string_view name, std::string_view fallback) -> std::string_view {
        const auto entry = options.find(name);
        return entry == options.end() ? fallback : std::string_view(entry->second);
    };
    const auto path = path_from_utf8(option("--db", "minetrack.sqlite"));
    if (command == "import") {
        const auto input = path_from_utf8(inputs.front());
        if (!fs::is_regular_file(input))
            throw std::runtime_error("Input file not found: " + path_utf8(input));
        const std::string extension = path_utf8(input.extension());
        if (!among(extension, {".csv", ".CSV", ".sql", ".sqlite", ".sqlite3", ".db"}))
            throw std::runtime_error("Expected .csv or an extracted SQLite file (.sql, .sqlite, .sqlite3, .db)");
        const auto source = local_source(input);
        Database db(path, true);
        if (extension == ".csv" || extension == ".CSV") db.import_csv(input, source);
        else db.import_sqlite(input, source);
        return 0;
    }

    const auto now = system_clock::now();
    const Integer candle_from = command == "candles" ? parse_time_bound(option("--from", "2016-10-01"), false) : 0;
    const Day from = command == "candles" ? Day{days{candle_from / day_ms}}
                                        : parse_day(option("--from", "2016-10-01"));
    // A UTC day is scheduled to appear at 01:00 UTC the following day.
    const Day default_to = command == "sync" ? floor<days>(now - hours{1}) - days{1} : floor<days>(now);
    const Integer candle_to = command == "candles"
        ? parse_time_bound(option("--to", iso_day(default_to)), true) : 0;
    const Day to = command == "candles" ? Day{days{(candle_to - 1) / day_ms}}
        : options.contains("--to") ? parse_day(options.at("--to")) : default_to;
    if (from > to || (command == "candles" && candle_from >= candle_to))
        throw std::runtime_error("--from must precede --to (a date-only --to includes the whole day)");
    if (command == "sync") {
        auto cache = path;
        cache += ".downloads";
        if (options.contains("--cache")) cache = path_from_utf8(options.at("--cache"));
        const Integer jobs = number(option("--jobs", "4"));
        if (jobs < 1 || jobs > 16) throw std::runtime_error("--jobs must be between 1 and 16");
        Database db(path, true);
        return sync_java(db, fs::absolute(cache), from, to, options.contains("--keep-downloads"),
                         static_cast<unsigned>(jobs));
    }

    const auto period = option("--period", command == "candles" ? "month" : "day");
    if (command != "candles" && !among(period, {"day", "month", "year"}))
        throw std::runtime_error("--period must be day, month or year");
    const Integer limit = number(option("--limit", command == "rank" ? "20" : command == "candles" ? "0" : "1000"));
    if (limit < 0) throw std::runtime_error("--limit must be nonnegative");
    std::optional<fs::path> output;
    if (options.contains("--output")) {
        output = path_from_utf8(options.at("--output"));
        protect_database(path, *output);
    }
    std::string extension = output ? path_utf8(output->extension()) : "";
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const bool all_methods = command == "candles" && option("--method", "index") == "all";
    const bool chart_directory = options.contains("--chart-dir");
    const bool with_csv = options.contains("--with-csv");
    const bool chart_set = options.contains("--chart-set");
    if (chart_directory && output)
        throw std::runtime_error("Use either --chart-dir for automatic names or --output for one file");
    if (all_methods && output)
        throw std::runtime_error("--method all uses --chart-dir, not a single --output file");
    if (chart_set && (output || options.contains("--period")))
        throw std::runtime_error("--chart-set chooses multiple periods; use --chart-dir without --period or --output");
    const bool png_path = command == "candles" && extension == ".png";
    const auto format = option("--format", png_path || chart_directory || all_methods || with_csv || chart_set ? "png" : output ? "csv" : "table");
    if (!among(format, {"table", "csv"}) && !(command == "candles" && format == "png"))
        throw std::runtime_error("--format must be table or csv; candles also supports png");
    if (command == "candles" && extension == ".svg")
        throw std::runtime_error("Use a .png output path for charts");
    if (format == "png" && output && extension != ".png")
        throw std::runtime_error("Explicit PNG output requires a .png path, or omit --output for automatic names");
    if ((all_methods || chart_directory || chart_set) && format != "png")
        throw std::runtime_error("--method all, --chart-set and --chart-dir require PNG output");
    if (with_csv && format != "png")
        throw std::runtime_error("--with-csv requires PNG output; use --format csv for CSV alone");
#ifdef _WIN32
    if (format == "csv" && !output) _setmode(_fileno(stdout), _O_BINARY);
#endif
    const Database db(path, false);
    if (command == "candles") {
        const auto host = option("--host", "");
        if (host.empty()) throw std::runtime_error("candles requires --host");
        const auto method = option("--method", "index");
        if (!all_methods && std::find(candle_methods.begin(), candle_methods.end(), method) == candle_methods.end())
            throw std::runtime_error("Unknown candle method; run 'minetrack methods'");
        const Integer span = number(option("--span", "7"));
        if (span < 1 || span > (std::numeric_limits<Integer>::max() - 5000) / 20)
            throw std::runtime_error("--span must be positive and fit the filter warm-up calculation");
        if (among(method, {"mean", "none"}) && options.contains("--span"))
            throw std::runtime_error("--span does not apply to mean or none");
        const double range_scale = real_number(option("--range-scale", "0.5"));
        if (range_scale <= 0 || range_scale > 10)
            throw std::runtime_error("--range-scale must be greater than zero and at most 10");
        if (!all_methods && method != "index" && options.contains("--range-scale"))
            throw std::runtime_error("--range-scale applies to --method index");
        struct Job { CandleOptions options; Integer from; std::string tag; };
        std::vector<Job> selections;
        const auto select = [&](std::string_view selected, const Interval& timeframe,
                                 Integer job_from, std::string tag) {
            Interval base = default_base(timeframe);
            if (selected == "index") {
                const bool calendar = timeframe.unit == Interval::Unit::month ||
                    timeframe.unit == Interval::Unit::year || timeframe.unit == Interval::Unit::week;
                base = calendar || timeframe.milliseconds() >= day_ms ? parse_interval("day") : timeframe;
            }
            if (options.contains("--base")) base = parse_interval(options.at("--base"), true);
            if (selected == "none") {
                if (!all_methods && options.contains("--base") && base.unit != Interval::Unit::raw)
                    throw std::runtime_error("--method none uses raw samples; --base must be raw or omitted");
                base = parse_interval("raw", true);
            }
            validate_base(timeframe, base);
            selections.push_back({{timeframe, base, std::string(selected), span, limit, range_scale}, job_from, std::move(tag)});
        };
        const auto select_methods = [&](const Interval& timeframe, Integer job_from, const std::string& tag) {
            if (all_methods) for (const auto selected : candle_methods) select(selected, timeframe, job_from, tag);
            else select(method, timeframe, job_from, tag);
        };
        if (chart_set) {
            const auto day_interval = parse_interval("day");
            const auto extent = db.candles(host, candle_from, candle_to,
                {day_interval, day_interval, "index", 7, 0, range_scale});
            if (extent.empty()) throw std::runtime_error("No positive samples in the selected date range");
            const year_month_day last{Day{days{extent.back().last / day_ms}}};
            const auto ms = [&](year_month_day date) {
                return std::max(candle_from, Day{date}.time_since_epoch().count() * day_ms);
            };
            const Integer year_start = ms(last.year()/January/1);
            const Integer years_start = ms((last.year() - years{2})/January/1);
            const auto recent_month = last.year()/last.month() - months{2};
            const Integer months_start = ms(recent_month/1);
            for (const auto* target : {"year", "month", "week", "day"}) {
                const auto timeframe = parse_interval(target);
                select_methods(timeframe, candle_from, "all");
                select_methods(timeframe, years_start, "last_3y");
                select_methods(timeframe, year_start, "year_" + std::to_string(int(last.year())));
                if (std::string_view(target) == "day") select_methods(timeframe, months_start, "last_3mo");
            }
        } else select_methods(parse_interval(period), candle_from, "");
        std::optional<fs::path> font_path;
        if (options.contains("--font")) font_path = path_from_utf8(options.at("--font"));
        const auto directory = path_from_utf8(option("--chart-dir", "charts"));
        std::size_t saved = 0;
        for (const auto& job : selections) {
            const auto& candle_options = job.options;
            if (all_methods || chart_set)
                std::cerr << '[' << (saved + 1) << '/' << selections.size() << "] "
                          << candle_options.method << ' ' << candle_options.period.spec << ' ' << job.tag
                          << ": computing and rendering\n";
            try {
                const auto candles = db.candles(host, job.from, candle_to, candle_options);
                if (candles.empty()) throw std::runtime_error("No positive samples in the selected date range");
                auto destination = output;
                if (format == "png" && !destination) {
                    protect_database(path, directory);
                    fs::create_directories(directory);
                    destination = automatic_chart_path(directory, host, candle_options, candles, with_csv, job.tag);
                    protect_database(path, *destination);
                }
                if (with_csv) {
                    auto csv = *destination;
                    csv.replace_extension(".csv");
                    protect_database(path, csv);
                }
                write_candles(candles, host, candle_options, format, destination, font_path, output.has_value(), with_csv);
                ++saved;
            } catch (const std::exception& error) {
                if (!all_methods && !chart_set) throw;
                throw std::runtime_error("Batch stopped at " + candle_options.method + " after " +
                    std::to_string(saved) + " saved PNG(s): " + error.what());
            }
        }
        if (all_methods || chart_set)
            std::cerr << "Saved " << saved << (chart_set ? " chart-set images to " : " method charts to ")
                      << path_utf8(fs::absolute(directory)) << '\n';
        return 0;
    }
    db.report(command, from, to, option("--host", ""), period, limit == 0 ? -1 : limit, format, output);
    return 0;
}

int entry(const std::vector<std::string>& args) {
    try {
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "minetrack: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

Day parse_day(std::string_view date) {
    const auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    if (date.size() != 10 || date[4] != '-' || date[7] != '-' ||
        !std::all_of(date.begin(), date.begin() + 4, digit) ||
        !std::all_of(date.begin() + 5, date.begin() + 7, digit) ||
        !std::all_of(date.begin() + 8, date.end(), digit)) {
        throw std::runtime_error("Expected YYYY-MM-DD: " + std::string(date));
    }
    const auto y = static_cast<int>(number(date.substr(0, 4)));
    const auto m = static_cast<unsigned>(number(date.substr(5, 2)));
    const auto d = static_cast<unsigned>(number(date.substr(8, 2)));
    const year_month_day value = year{y}/m/d;
    if (y < 1970 || !value.ok()) throw std::runtime_error("Invalid UTC date: " + std::string(date));
    return Day(value);
}

std::string iso_day(Day day) {
    const year_month_day date{day};
    const auto padded = [](unsigned value) { return (value < 10 ? "0" : "") + std::to_string(value); };
    return std::to_string(int(date.year())) + "-" + padded(unsigned(date.month())) + "-" +
        padded(unsigned(date.day()));
}

fs::path path_from_utf8(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}

std::string path_utf8(const fs::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(path_utf8(fs::path(argv[i])));
    return entry(args);
}
#else
int main(int argc, char** argv) {
    return entry(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
