#include "minetrack.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {

struct Color { unsigned char r, g, b; };
constexpr Color green{19, 135, 119}, red{220, 65, 74};
constexpr Color ink{23, 37, 56}, muted{83, 96, 117}, grid{237, 240, 244};

struct Canvas {
    int width, height;
    std::vector<unsigned char> pixels;
    Canvas(int w, int h) : width(w), height(h), pixels(static_cast<std::size_t>(w) * h * 3, 255) {}
    void pixel(int x, int y, Color color, double alpha) {
        if (x < 0 || x >= width || y < 0 || y >= height || alpha <= 0) return;
        auto* p = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 3;
        const unsigned char rgb[]{color.r, color.g, color.b};
        for (int i = 0; i < 3; ++i)
            p[i] = static_cast<unsigned char>(std::lround(p[i] * (1 - alpha) + rgb[i] * alpha));
    }
    // Fractional pixel coverage keeps dense candles and thin wicks visible.
    void rectangle(double left, double top, double right, double bottom, Color color) {
        left = std::clamp(left, 0.0, static_cast<double>(width));
        right = std::clamp(right, 0.0, static_cast<double>(width));
        top = std::clamp(top, 0.0, static_cast<double>(height));
        bottom = std::clamp(bottom, 0.0, static_cast<double>(height));
        for (int y = static_cast<int>(std::floor(top)); y < static_cast<int>(std::ceil(bottom)); ++y)
            for (int x = static_cast<int>(std::floor(left)); x < static_cast<int>(std::ceil(right)); ++x) {
                const double coverage = std::max(0.0, std::min(right, x + 1.0) - std::max(left, double(x))) *
                    std::max(0.0, std::min(bottom, y + 1.0) - std::max(top, double(y)));
                pixel(x, y, color, coverage);
            }
    }
};

int next_codepoint(std::string_view text, std::size_t& i) {
    const unsigned char first = text[i++];
    if (first < 128) return first;
    int extra = first < 224 ? 1 : first < 240 ? 2 : 3;
    int value = first & (extra == 1 ? 31 : extra == 2 ? 15 : 7);
    while (extra-- && i < text.size()) value = (value << 6) | (static_cast<unsigned char>(text[i++]) & 63);
    return value;
}

class Font {
public:
    explicit Font(const std::optional<fs::path>& requested) {
        fs::path path;
        if (requested) path = *requested;
        else {
            for (const auto* candidate : {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/segoeui.ttf",
                     "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                     "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                     "/System/Library/Fonts/Supplemental/Arial.ttf"}) {
                if (fs::is_regular_file(candidate)) { path = candidate; break; }
            }
        }
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Cannot open a system font; specify --font with a TTF/TTC/OTF file");
        const auto size = file.tellg();
        if (size < 12 || size > 128 * 1024 * 1024) throw std::runtime_error("Invalid font file size");
        bytes_.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes_.data()), size);
        const int offset = stbtt_GetFontOffsetForIndex(bytes_.data(), 0);
        if (!file || offset < 0 || !stbtt_InitFont(&font_, bytes_.data(), offset))
            throw std::runtime_error("Cannot load font: " + path_utf8(path));
        stbtt_GetFontVMetrics(&font_, &ascent_, nullptr, nullptr);
    }
    bool has(int codepoint) const { return stbtt_FindGlyphIndex(&font_, codepoint) != 0; }
    double measure(std::string_view text, double height) const {
        const double scale = stbtt_ScaleForPixelHeight(&font_, static_cast<float>(height));
        double width = 0;
        int previous = 0;
        for (std::size_t i = 0; i < text.size();) {
            const int cp = next_codepoint(text, i);
            int advance;
            stbtt_GetCodepointHMetrics(&font_, cp, &advance, nullptr);
            width += (advance + stbtt_GetCodepointKernAdvance(&font_, previous, cp)) * scale;
            previous = cp;
        }
        return width;
    }
    void text(Canvas& canvas, std::string_view text, double x, double top, double height, Color color) const {
        const float scale = stbtt_ScaleForPixelHeight(&font_, static_cast<float>(height));
        const int baseline = static_cast<int>(std::lround(top + ascent_ * scale));
        int previous = 0;
        for (std::size_t i = 0; i < text.size();) {
            const int cp = next_codepoint(text, i);
            x += stbtt_GetCodepointKernAdvance(&font_, previous, cp) * scale;
            int w, h, dx, dy, advance;
            auto* bitmap = stbtt_GetCodepointBitmap(&font_, scale, scale, cp, &w, &h, &dx, &dy);
            if (bitmap) {
                for (int row = 0; row < h; ++row)
                    for (int col = 0; col < w; ++col)
                        canvas.pixel(static_cast<int>(std::lround(x)) + dx + col, baseline + dy + row,
                                     color, bitmap[row * w + col] / 255.0);
                stbtt_FreeBitmap(bitmap, nullptr);
            }
            stbtt_GetCodepointHMetrics(&font_, cp, &advance, nullptr);
            x += advance * scale;
            previous = cp;
        }
    }
private:
    std::vector<unsigned char> bytes_;
    stbtt_fontinfo font_{};
    int ascent_ = 0;
};

std::string number_label(double value) {
    std::ostringstream out;
    out << std::setprecision(5) << value;
    return out.str();
}

void png_write(void* context, void* data, int length) {
    // No exception may escape through the vendor callback.
    static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data), length);
}

} // namespace

void render_candle_png(const std::vector<Candle>& candles, std::string_view host,
                       const CandleOptions& options, const fs::path& output,
                       const std::optional<fs::path>& font_path) {
    if (candles.empty()) throw std::runtime_error("Cannot render an empty candle series");
    const int multiplier = candles.size() > 2000 ? 2 : 1;
    Canvas canvas(2880 * multiplier, 1440 * multiplier);
    Font font(font_path);
    const double s = multiplier;
    const double left = 130 * s, right = canvas.width - 25 * s;
    const double top = 72 * s, bottom = canvas.height - 78 * s;
    double low = candles.front().low, high = candles.front().high;
    for (const auto& c : candles) {
        if (!std::isfinite(c.low) || !std::isfinite(c.high) || !std::isfinite(c.open) ||
            !std::isfinite(c.close) || c.low <= 0 || c.low > std::min(c.open, c.close) ||
            c.high < std::max(c.open, c.close) || c.begin >= c.end)
            throw std::runtime_error("Invalid candle values for PNG rendering");
        low = std::min(low, c.low);
        high = std::max(high, c.high);
    }
    const double divisor = high >= 10000 ? 10000 : 1;
    const double padding = std::max((high - low) * 0.04, high * 0.005);
    low = std::max(0.0, low - padding);
    high += padding;
    const auto begin = candles.front().begin, end = candles.back().end;
    const auto x = [&](Integer time) { return left + double(time - begin) / double(end - begin) * (right - left); };
    const auto y = [&](double value) { return bottom - (value - low) / (high - low) * (bottom - top); };
    const double rough = (high - low) / 6;
    const double decade = std::pow(10.0, std::floor(std::log10(rough)));
    const double factor = rough / decade;
    const double step = (factor <= 1 ? 1 : factor <= 2 ? 2 : factor <= 2.5 ? 2.5 : factor <= 5 ? 5 : 10) * decade;
    for (double value = std::ceil(low / step) * step; value <= high; value += step) {
        const double py = y(value);
        canvas.rectangle(left, py, right, py + s, grid);
        const auto label = number_label(value / divisor);
        font.text(canvas, label, left - 20 * s - font.measure(label, 24 * s), py - 14 * s, 24 * s, muted);
    }
    canvas.rectangle(left - s, top, left, bottom + s, {217, 224, 233});
    canvas.rectangle(left, bottom, right, bottom + s, {217, 224, 233});
    const bool intraday = end - begin < 3 * day_ms;
    for (int i = 0; i <= 6; ++i) {
        const Integer time = begin + (end - begin) * i / 6;
        const auto iso = iso_time(time);
        const auto label = end - begin < 60000 ? iso.substr(11, iso.size() - 12) :
                           intraday ? iso.substr(5, 11) : iso.substr(0, 10);
        const double width = font.measure(label, 23 * s);
        const double px = std::clamp(x(time) - width / 2, left, right - width);
        font.text(canvas, label, px, bottom + 20 * s, 23 * s, muted);
    }
    for (const auto& c : candles) {
        const double start = x(c.begin), stop = x(c.end);
        const double center = (start + stop) / 2, width = (stop - start) * 0.72;
        const Color color = c.close >= c.open ? green : red;
        const double wick = std::max(0.25 * s, std::min(1.7 * s, (stop - start) * 0.22));
        canvas.rectangle(center - wick / 2, y(c.high), center + wick / 2, y(c.low), color);
        const double body_top = y(std::max(c.open, c.close)), body_bottom = y(std::min(c.open, c.close));
        if (body_bottom - body_top < s)
            canvas.rectangle(center - width / 2, body_top - s / 2, center + width / 2, body_top + s / 2, color);
        else canvas.rectangle(center - width / 2, body_top, center + width / 2, body_bottom, color);
    }
    const bool chinese = font.has(0x6da8) && font.has(0x8dcc);
    std::string method = options.method;
    std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) { return std::toupper(c); });
    std::string title = std::string(host) + "  |  " + options.period.spec + " K  |  " + method;
    if (options.method != "none" && options.method != "mean") title += std::to_string(options.span);
    title += divisor == 10000 ? (chinese ? "  |  万人" : "  |  x10,000 players") : (chinese ? "  |  人" : "  |  players");
    const std::string dates = iso_time(candles.front().first).substr(0, 10) + " - " +
                              iso_time(candles.back().last).substr(0, 10) + " UTC";
    const double date_width = font.measure(dates, 23 * s);
    const double available = right - left - date_width - 45 * s;
    while (font.measure(title, 30 * s) > available && !title.empty()) {
        std::size_t pos = title.size() - 1;
        while (pos > 0 && (static_cast<unsigned char>(title[pos]) & 0xc0) == 0x80) --pos;
        title.erase(pos);
    }
    font.text(canvas, title, left, 18 * s, 30 * s, ink);
    font.text(canvas, dates, right - date_width, 22 * s, 23 * s, muted);
    const auto up = chinese ? "涨" : "UP", down = chinese ? "跌" : "DOWN";
    const double legend_y = top + 18 * s;
    canvas.rectangle(right - 180 * s, legend_y, right - 156 * s, legend_y + 14 * s, green);
    font.text(canvas, up, right - 145 * s, legend_y - 4 * s, 22 * s, ink);
    canvas.rectangle(right - 85 * s, legend_y, right - 61 * s, legend_y + 14 * s, red);
    font.text(canvas, down, right - 50 * s, legend_y - 4 * s, 22 * s, ink);
    std::ofstream file(output, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot create PNG output: " + path_utf8(output));
    if (!stbi_write_png_to_func(png_write, &file, canvas.width, canvas.height, 3,
                                 canvas.pixels.data(), canvas.width * 3))
        throw std::runtime_error("PNG encoding failed");
    file.close();
    if (!file) throw std::runtime_error("Could not write PNG output: " + path_utf8(output));
}
