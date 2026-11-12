#include "smoothing.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

void RollingAverage::add(double value) {
    if (values_.size() == length_) {
        weighted_ -= sum_;
        sum_ -= values_.front();
        values_.pop_front();
    }
    values_.push_back(value);
    sum_ += value;
    weighted_ += static_cast<long double>(values_.size()) * value;
}

double RollingAverage::mean() const {
    return static_cast<double>(sum_ / values_.size());
}

double RollingAverage::weighted() const {
    const long double n = values_.size();
    return static_cast<double>(weighted_ / (n * (n + 1) / 2));
}

Smoother::Smoother(std::string_view method, std::size_t span)
    : method_(method), span_(span), whole_(span), half_(std::max<std::size_t>(1, span / 2)),
      root_(std::max<std::size_t>(1, static_cast<std::size_t>(std::sqrt(span)))) {}

void Smoother::rebalance() {
    while (lower_.size() > upper_.size() + 1) {
        auto it = std::prev(lower_.end());
        upper_.insert(*it);
        lower_.erase(it);
    }
    while (lower_.size() < upper_.size()) {
        auto it = upper_.begin();
        lower_.insert(*it);
        upper_.erase(it);
    }
}

double Smoother::add(double value) {
    if (method_ == "none" || method_ == "mean") return value;
    if (method_ == "sma" || method_ == "wma" || method_ == "hma") {
        whole_.add(value);
        if (method_ == "sma") return whole_.mean();
        if (method_ == "wma") return whole_.weighted();
        half_.add(value);
        root_.add(2 * half_.weighted() - whole_.weighted());
        return std::max(1e-9, root_.weighted());
    }
    if (method_ == "median") {
        if (lower_.empty() || value <= *lower_.rbegin()) lower_.insert(value);
        else upper_.insert(value);
        window_.push_back(value);
        if (window_.size() > span_) {
            auto it = lower_.find(window_.front());
            if (it != lower_.end()) lower_.erase(it);
            else upper_.erase(upper_.find(window_.front()));
            window_.pop_front();
        }
        rebalance();
        return lower_.size() == upper_.size()
            ? *lower_.rbegin() / 2 + *upper_.begin() / 2 : *lower_.rbegin();
    }
    if (method_ == "kama") {
        if (!window_.empty()) movement_ += std::abs(value - window_.back());
        window_.push_back(value);
        if (window_.size() > span_ + 1) {
            movement_ -= std::abs(window_[1] - window_[0]);
            window_.pop_front();
        }
        const double efficiency = movement_ > 0
            ? std::clamp(static_cast<double>(std::abs(value - window_.front()) / movement_), 0.0, 1.0) : 0;
        const double rate = efficiency * (2.0 / 3 - 2.0 / 31) + 2.0 / 31;
        if (!seeded_) first_ = value;
        else first_ += rate * rate * (value - first_);
        seeded_ = true;
        return first_;
    }
    const double alpha = method_ == "rma" ? 1.0 / span_ : 2.0 / (span_ + 1.0);
    if (!seeded_) {
        first_ = second_ = third_ = value;
        seeded_ = true;
    } else {
        first_ += alpha * (value - first_);
        second_ += alpha * (first_ - second_);
        third_ += alpha * (second_ - third_);
    }
    if (method_ == "dema") return std::max(1e-9, 2 * first_ - second_);
    if (method_ == "tema") return std::max(1e-9, 3 * first_ - 3 * second_ + third_);
    return first_;
}
