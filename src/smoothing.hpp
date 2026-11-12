#pragma once

#include <cstddef>
#include <deque>
#include <set>
#include <string>
#include <string_view>

// Online filters retain only their current window; no full-history arrays.
class RollingAverage {
public:
    explicit RollingAverage(std::size_t length) : length_(length) {}
    void add(double value);
    double mean() const;
    double weighted() const;
private:
    std::size_t length_;
    std::deque<double> values_;
    long double sum_ = 0, weighted_ = 0;
};

class Smoother {
public:
    Smoother(std::string_view method, std::size_t span);
    double add(double value);
private:
    void rebalance();
    std::string method_;
    std::size_t span_;
    RollingAverage whole_, half_, root_;
    std::deque<double> window_;
    std::multiset<double> lower_, upper_;
    long double movement_ = 0;
    double first_ = 0, second_ = 0, third_ = 0;
    bool seeded_ = false;
};
