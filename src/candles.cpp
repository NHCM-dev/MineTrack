#include "minetrack.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

CandleBuilder::CandleBuilder(const CandleOptions& options, Integer from)
    : options_(options), from_(from), first_period_(options.period.bounds(from).first),
      smoother_(options.method, static_cast<std::size_t>(options.span)) {}

void CandleBuilder::add(const MeanPoint& point) {
    if (options_.method == "index") {
        add_index(point);
        return;
    }
    const double value = smoother_.add(point.players);
    const auto [begin, end] = options_.period.bounds(point.begin);
    if (point.end <= from_ && begin == first_period_) return;
    if (pending_ && pending_->begin != begin) finish_index();
    if (!pending_) {
        const double open = previous_close_.value_or(value);
        pending_ = Candle{options_.period.label(begin), begin, end, point.begin, point.end - 1,
                          open, std::max(open, value), std::min(open, value), value, 0, 0, 0};
        last_day_ = -1;
    }
    Candle& candle = *pending_;
    candle.last = point.end - 1;
    candle.high = std::max(candle.high, value);
    candle.low = std::min(candle.low, value);
    candle.close = value;
    for (Integer day = point.first_sample / day_ms; day <= point.last_sample / day_ms; ++day) {
        if (day > last_day_) {
            ++candle.days;
            last_day_ = day;
        }
    }
    ++candle.points;
    candle.samples += point.samples;
}

void CandleBuilder::store(Candle candle) {
    if (candle.last < from_) return;
    candles_.push_back(std::move(candle));
    if (options_.limit > 0 && candles_.size() > static_cast<std::size_t>(options_.limit))
        candles_.pop_front();
}

void CandleBuilder::add_index(const MeanPoint& point) {
    const auto [begin, end] = options_.period.bounds(point.begin);
    // Warm-up uses complete earlier candles, while a clipped first candle
    // contains only samples in the requested range.
    if (point.end <= from_ && begin == first_period_) return;
    if (pending_ && pending_->begin != begin) finish_index();
    if (!pending_) {
        pending_ = Candle{options_.period.label(begin), begin, end, point.begin, point.end - 1,
                          0, 0, 0, 0, 0, 0, 0};
        period_sum_ = 0;
        last_day_ = -1;
    }
    pending_->last = point.end - 1;
    period_sum_ += static_cast<long double>(point.players) * point.samples;
    pending_->samples += point.samples;
    ++pending_->points;
    for (Integer day = point.first_sample / day_ms; day <= point.last_sample / day_ms; ++day) {
        if (day > last_day_) {
            ++pending_->days;
            last_day_ = day;
        }
    }
}

void CandleBuilder::finish_index() {
    if (!pending_) return;
    Candle candle = *pending_;
    if (options_.method != "index") {
        previous_close_ = candle.close;
        store(std::move(candle));
        pending_.reset();
        return;
    }
    candle.source_mean = static_cast<double>(period_sum_ / candle.samples);
    const double level = std::log(candle.source_mean);
    if (!index_seeded_) {
        index_log_ = level;
        index_seeded_ = true;
        candle.open = candle.high = candle.low = candle.close = std::exp(index_log_);
    } else {
        const double alpha = 2.0 / (static_cast<double>(options_.span) + 1.0);
        const double residual = level - index_log_;
        const double scale = std::max(minimum_log_scale, std::sqrt(index_variance_));
        const double limit = innovation_clip_sigma * scale;
        const double clipped = std::clamp(residual, -limit, limit);
        candle.open = *previous_close_;
        index_log_ += alpha * clipped;
        candle.close = std::exp(index_log_);
        const double deviation = std::clamp(level - index_log_, -limit, limit);
        upper_deviation_ += alpha * (std::max(deviation, 0.0) - upper_deviation_);
        lower_deviation_ += alpha * (std::max(-deviation, 0.0) - lower_deviation_);
        const double width = alpha * options_.range_scale;
        candle.high = std::max(candle.open, candle.close) * std::exp(width * upper_deviation_);
        candle.low = std::min(candle.open, candle.close) * std::exp(-width * lower_deviation_);
        index_variance_ = std::max(0.0, index_variance_ + alpha * (clipped * clipped - index_variance_));
    }
    previous_close_ = candle.close;
    store(std::move(candle));
    pending_.reset();
}

std::vector<Candle> CandleBuilder::finish() {
    finish_index();
    return {candles_.begin(), candles_.end()};
}

namespace {

void candle_table(std::ostream& out, const std::vector<Candle>& candles, std::string_view host,
                  const CandleOptions& options, bool csv) {
    if (csv) {
        out << "period,host,method,span,open,high,low,close,days,samples,first_date,last_date,"
               "timeframe,base,start_time,end_time,first_time,last_time,points,source_mean,range_scale,algorithm\r\n"
            << std::setprecision(17);
        for (const auto& candle : candles) {
            out << candle.period << ',' << host << ',' << options.method << ','
                << (options.method != "mean" && options.method != "none" ? options.span : 0) << ','
                << candle.open << ',' << candle.high << ',' << candle.low << ',' << candle.close << ','
                << candle.days << ',' << candle.samples << ',' << iso_time(candle.first).substr(0, 10) << ','
                << iso_time(candle.last).substr(0, 10) << ',' << options.period.spec << ',' << options.base.spec << ','
                << iso_time(candle.begin) << ',' << iso_time(candle.end) << ',' << iso_time(candle.first) << ','
                << iso_time(candle.last) << ',' << candle.points << ',';
            if (options.method == "index")
                out << candle.source_mean << ',' << options.range_scale << ",mean-envelope-v1";
            else out << ",,";
            out << "\r\n";
        }
        return;
    }
    out << "period                        open          high           low         close    points\n"
        << std::fixed << std::setprecision(2);
    for (const auto& candle : candles)
        out << std::left << std::setw(20) << candle.period << std::right << "  " << std::setw(12) << candle.open << "  " << std::setw(12) << candle.high
            << "  " << std::setw(12) << candle.low << "  " << std::setw(12) << candle.close
            << "  " << std::setw(6) << candle.points << '\n';
}

class ChartStaging {
public:
    explicit ChartStaging(const fs::path& parent) {
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::ostringstream name;
            name << ".minetrack-chart-" << std::hex << random() << random();
            const auto candidate = parent / name.str();
            if (fs::create_directory(candidate)) {
                folder = candidate;
                return;
            }
        }
        throw std::runtime_error("Could not create a unique chart staging directory");
    }
    ~ChartStaging() {
        std::error_code error;
        if (!preserve) fs::remove_all(folder, error);
    }
    ChartStaging(const ChartStaging&) = delete;
    ChartStaging& operator=(const ChartStaging&) = delete;
    fs::path folder;
    bool preserve = false;
};

void png_chart(const std::vector<Candle>& candles, std::string_view host,
               const CandleOptions& options, const fs::path& output,
               const std::optional<fs::path>& font_path, bool replace_output, bool with_csv) {
    const auto destination = fs::absolute(output);
    ChartStaging staging(destination.parent_path());
    std::vector<fs::path> targets{destination};
    std::vector<fs::path> prepared{staging.folder / "chart.png"};
    render_candle_png(candles, host, options, prepared[0], font_path);
    if (with_csv) {
        auto csv_target = destination;
        csv_target.replace_extension(".csv");
        targets.push_back(csv_target);
        prepared.push_back(staging.folder / "data.csv");
        std::ofstream data(prepared.back(), std::ios::binary);
        if (!data) throw std::runtime_error("Could not open OHLC CSV output");
        candle_table(data, candles, host, options, true);
        data.close();
        if (!data) throw std::runtime_error("Could not write OHLC CSV output");
    }
    std::vector<fs::path> backups(targets.size());
    std::vector<bool> published(targets.size(), false);
    try {
        // Prepare every file before touching an existing output. Backups permit
        // rollback if the second file of a PNG/CSV pair cannot be published.
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (!fs::exists(fs::symlink_status(targets[i]))) continue;
            if (!replace_output) throw std::runtime_error("Output already exists: " + path_utf8(targets[i]));
            if (fs::is_directory(targets[i])) throw std::runtime_error("Output is a directory: " + path_utf8(targets[i]));
            auto backup = staging.folder / ("backup-" + std::to_string(i));
            fs::rename(targets[i], backup);
            backups[i] = backup;
        }
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (replace_output) fs::rename(prepared[i], targets[i]);
            else fs::copy_file(prepared[i], targets[i], fs::copy_options::none);
            published[i] = true;
        }
    } catch (...) {
        for (std::size_t i = targets.size(); i-- > 0;) {
            std::error_code error;
            if (published[i]) fs::remove(targets[i], error);
            if (!backups[i].empty()) {
                fs::rename(backups[i], targets[i], error);
                if (error) {
                    staging.preserve = true;
                    std::cerr << "Output backup retained at " << path_utf8(backups[i]) << '\n';
                }
            }
        }
        throw;
    }
    std::cerr << "Saved PNG: " << path_utf8(output) << '\n';
    if (with_csv) std::cerr << "Saved CSV: " << path_utf8(targets[1]) << '\n';
}

} // namespace

void write_candles(const std::vector<Candle>& candles, std::string_view host,
                   const CandleOptions& options, std::string_view format,
                   const std::optional<fs::path>& output, const std::optional<fs::path>& font_path,
                   bool replace_output, bool with_csv) {
    if (format == "png") {
        if (!output) throw std::runtime_error("PNG output requires --output");
        png_chart(candles, host, options, *output, font_path, replace_output, with_csv);
        return;
    }
    std::ofstream file;
    if (output) {
        file.open(*output, std::ios::binary);
        if (!file) throw std::runtime_error("Could not open chart output " + path_utf8(*output));
    }
    std::ostream& out = output ? file : std::cout;
    candle_table(out, candles, host, options, format == "csv");
    out.flush();
    if (!out) throw std::runtime_error("Could not write candle output");
    if (output) {
        file.close();
        if (!file) throw std::runtime_error("Could not close candle output");
    }
}
