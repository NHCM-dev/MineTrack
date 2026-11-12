#pragma once

#include "smoothing.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct sqlite3;
namespace fs = std::filesystem;
using Day = std::chrono::sys_days;
using Integer = std::int64_t;

inline constexpr Integer day_ms = 86'400'000;
inline constexpr int application_id = 0x4d54524b; // MTRK
inline constexpr Day first_day = std::chrono::year{2016}/10/1;
inline constexpr Day first_daily = std::chrono::year{2020}/8/1;
inline constexpr Day last_supported_day = std::chrono::year{9999}/12/31;

Day parse_day(std::string_view date);
std::string iso_day(Day day);
fs::path path_from_utf8(std::string_view value);
std::string path_utf8(const fs::path& path);

struct Interval {
    enum class Unit { raw, second, minute, hour, day, week, month, year };
    Unit unit;
    Integer count;
    std::string spec;
    Integer milliseconds() const;
    std::pair<Integer, Integer> bounds(Integer timestamp) const;
    std::string label(Integer start) const;
    std::string chinese() const;
};

Interval parse_interval(std::string_view value, bool allow_raw = false);
Interval default_base(const Interval& period);
void validate_base(const Interval& period, const Interval& base);
Integer parse_time_bound(std::string_view value, bool end);
std::string iso_time(Integer timestamp);

struct MeanPoint {
    Integer begin;
    Integer end;
    double players;
    Integer samples;
    Integer first_sample;
    Integer last_sample;
};

struct Candle {
    std::string period;
    Integer begin;
    Integer end;
    Integer first;
    Integer last;
    double open;
    double high;
    double low;
    double close;
    Integer days;
    Integer samples;
    Integer points;
    double source_mean = 0;
};

struct CandleOptions {
    Interval period;
    Interval base;
    std::string method;
    Integer span;
    Integer limit;
    double range_scale = 0.5;
};

class CandleBuilder {
public:
    CandleBuilder(const CandleOptions& options, Integer from);
    void add(const MeanPoint& point);
    std::vector<Candle> finish();
private:
    static constexpr double minimum_log_scale = 0.02;
    static constexpr double innovation_clip_sigma = 3;
    void add_index(const MeanPoint& point);
    void finish_index();
    void store(Candle candle);
    const CandleOptions& options_;
    Integer from_;
    Integer first_period_;
    std::optional<double> previous_close_;
    Smoother smoother_;
    Integer last_day_ = -1;
    std::deque<Candle> candles_;
    std::optional<Candle> pending_;
    long double period_sum_ = 0;
    bool index_seeded_ = false;
    double index_log_ = 0;
    double index_variance_ = minimum_log_scale * minimum_log_scale;
    double upper_deviation_ = 0;
    double lower_deviation_ = 0;
};

void write_candles(const std::vector<Candle>& candles, std::string_view host,
                   const CandleOptions& options, std::string_view format,
                   const std::optional<fs::path>& output, const std::optional<fs::path>& font_path = std::nullopt,
                   bool replace_output = true, bool with_csv = false);
void render_candle_png(const std::vector<Candle>& candles, std::string_view host,
                       const CandleOptions& options, const fs::path& output,
                       const std::optional<fs::path>& font_path);

int run_tool(const std::vector<std::string>& arguments, const fs::path& stdout_file);

class Database {
public:
    Database(const fs::path& path, bool writable);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool imported(std::string_view source) const;
    void import_csv(const fs::path& path, const std::string& source);
    void import_sqlite(const fs::path& path, const std::string& source);
    std::vector<Candle> candles(std::string_view host, Integer from, Integer to,
                                const CandleOptions& options) const;
    void report(std::string_view command, Day from, Day to, std::string_view host,
                std::string_view period, Integer limit, std::string_view format,
                const std::optional<fs::path>& output) const;
private:
    sqlite3* db_ = nullptr;
};

std::string local_source(const fs::path& path);
int sync_java(Database& db, const fs::path& cache, Day from, Day to, bool keep_downloads,
              unsigned download_jobs);
