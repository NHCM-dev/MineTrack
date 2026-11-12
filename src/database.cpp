#include "minetrack.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {

[[noreturn]] void sql_error(sqlite3* db, std::string_view action) {
    throw std::runtime_error(std::string(action) + ": " + sqlite3_errmsg(db));
}

using Connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

Connection connect(const fs::path& path, int flags) {
    sqlite3* handle = nullptr;
    // Each connection is used by one CLI thread.
    const int status = sqlite3_open_v2(path_utf8(path).c_str(), &handle, flags | SQLITE_OPEN_NOMUTEX, nullptr);
    Connection connection(handle, sqlite3_close);
    if (!handle) throw std::runtime_error("Could not allocate SQLite connection");
    if (status != SQLITE_OK) sql_error(handle, "Open " + path_utf8(path));
    sqlite3_busy_timeout(handle, 30'000);
    return connection;
}

void exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        sql_error(db, "Execute SQL");
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            sql_error(db, "Prepare SQL");
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int column, Integer value) {
        check(sqlite3_bind_int64(stmt_, column, value));
    }
    void bind(int column, std::string_view value) {
        check(sqlite3_bind_text64(stmt_, column, value.empty() ? "" : value.data(),
                                  value.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    }
    bool next() {
        switch (sqlite3_step(stmt_)) {
            case SQLITE_ROW: return true;
            case SQLITE_DONE: return false;
            default: sql_error(db_, "Step SQL");
        }
    }
    void run() {
        if (next()) throw std::runtime_error("Unexpected result row");
        reset();
    }
    void reset() { check(sqlite3_reset(stmt_)); }
    bool insert_unique() {
        const int status = sqlite3_step(stmt_);
        if (status == SQLITE_DONE) {
            check(sqlite3_reset(stmt_));
            return true;
        }
        if (sqlite3_extended_errcode(db_) != SQLITE_CONSTRAINT_PRIMARYKEY)
            sql_error(db_, "Insert sample batch");
        sqlite3_reset(stmt_); // The expected constraint error also comes back from reset.
        return false;
    }
    Integer integer(int column) const { return sqlite3_column_int64(stmt_, column); }
    int type(int column) const { return sqlite3_column_type(stmt_, column); }
    std::string_view string(int column) const {
        const auto* data = sqlite3_column_text(stmt_, column);
        if (!data) return {};
        return {reinterpret_cast<const char*>(data),
                static_cast<std::size_t>(sqlite3_column_bytes(stmt_, column))};
    }
    int columns() const { return sqlite3_column_count(stmt_); }
    std::string_view name(int column) const { return sqlite3_column_name(stmt_, column); }
private:
    void check(int status) const {
        if (status != SQLITE_OK) sql_error(db_, "Bind or reset SQL");
    }
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

Integer scalar(sqlite3* db, const char* sql) {
    Statement statement(db, sql);
    if (!statement.next()) throw std::runtime_error("Expected a SQL result");
    return statement.integer(0);
}

class Transaction {
public:
    explicit Transaction(sqlite3* db, bool writable = true) : db_(db) {
        exec(db_, writable ? "BEGIN IMMEDIATE" : "BEGIN");
    }
    ~Transaction() {
        if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() {
        exec(db_, "COMMIT");
        committed_ = true;
    }
private:
    sqlite3* db_;
    bool committed_ = false;
};

constexpr Integer legacy_zero_cutoff =
    Day(std::chrono::year{2020}/7/1).time_since_epoch().count() * day_ms;
constexpr Integer latest_timestamp = (last_supported_day.time_since_epoch().count() + 1) * day_ms - 1;
// Existing databases store NULL as -1. Shift source negatives down by one
// to preserve the original value without colliding with that encoding.
constexpr Integer missing_players = -1;
constexpr Integer no_minimum = std::numeric_limits<int>::max();

struct Progress {
    Integer rowid = 0;
    Integer scanned = 0;
    Integer added = 0;
    bool complete = false;
};

Progress progress(sqlite3* db, std::string_view source) {
    Statement query(db, "SELECT cursor, scanned, added, complete FROM imports WHERE source=?");
    query.bind(1, source);
    if (!query.next()) return {};
    return {query.integer(0), query.integer(1), query.integer(2), query.integer(3) != 0};
}

void save_progress(sqlite3* db, std::string_view source, const Progress& state) {
    Statement save(db, R"SQL(
        INSERT INTO imports(source, cursor, scanned, added, complete)
        VALUES(?, ?, ?, ?, ?)
        ON CONFLICT(source) DO UPDATE SET cursor=excluded.cursor, scanned=excluded.scanned,
            added=excluded.added, complete=excluded.complete
    )SQL");
    save.bind(1, source);
    save.bind(2, state.rowid);
    save.bind(3, state.scanned);
    save.bind(4, state.added);
    save.bind(5, state.complete);
    save.run();
}

struct Totals {
    Integer samples = 0;
    Integer failed = 0;
    Integer ambiguous = 0;
    Integer sum = 0;
    Integer low = no_minimum;
    Integer high = -1;
};

class Batch {
public:
    explicit Batch(sqlite3* db)
        : db_(db), add_host_(db, "INSERT INTO hosts(name) VALUES(?) ON CONFLICT(name) DO NOTHING"),
          add_chunk_(db, chunk_sql().c_str()),
          add_sample_(db, "INSERT INTO samples VALUES(?, ?, ?) ON CONFLICT(timestamp_ms, host_id, players) DO NOTHING") {}

    void add(Integer timestamp, std::string_view host, std::optional<Integer> count) {
        if (timestamp < 0 || timestamp > latest_timestamp ||
            (count && (*count < std::numeric_limits<std::int32_t>::min() || *count > no_minimum)))
            throw std::runtime_error("Invalid timestamp or player count");
        Integer players = count.value_or(missing_players);
        if (count && players < 0) --players;
        Host& entry = find_host(host);
        const Integer day = timestamp / day_ms;
        if (entry.day != day) {
            entry.day = day;
            entry.totals = &totals_[{day, entry.id}];
        }
        pending_[size_++] = {timestamp, entry.id, players, entry.totals};
        if (size_ == chunk_size) insert_pending();
    }

    Integer flush() {
        insert_pending();
        Statement save(db_, R"SQL(
            INSERT INTO daily VALUES(?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(day, host_id) DO UPDATE SET
                samples=daily.samples+excluded.samples, failed=daily.failed+excluded.failed,
                ambiguous=daily.ambiguous+excluded.ambiguous, players_sum=daily.players_sum+excluded.players_sum,
                low=min(daily.low, excluded.low), high=max(daily.high, excluded.high)
        )SQL");
        for (const auto& [key, totals] : totals_) {
            if (totals.samples == 0) continue;
            save.bind(1, key.first);
            save.bind(2, key.second);
            save.bind(3, totals.samples);
            save.bind(4, totals.failed);
            save.bind(5, totals.ambiguous);
            save.bind(6, totals.sum);
            save.bind(7, totals.low);
            save.bind(8, totals.high);
            save.run();
        }
        return added_;
    }

private:
    static constexpr int chunk_size = 256; // Three variables per row, below SQLite's older 999 limit.

    struct Host {
        Integer id;
        Integer day = -1;
        Totals* totals = nullptr;
    };

    struct Sample {
        Integer timestamp;
        Integer host_id;
        Integer players;
        Totals* totals;
    };

    struct HostHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view name) const {
            return std::hash<std::string_view>{}(name);
        }
    };

    static const std::string& chunk_sql() {
        static const std::string sql = [] {
            std::string query = "INSERT OR ABORT INTO samples VALUES ";
            for (int i = 0; i < chunk_size; ++i)
                query += i == 0 ? "(?,?,?)" : ",(?,?,?)";
            return query;
        }();
        return sql;
    }

    static void bind_sample(Statement& statement, int first, const Sample& sample) {
        statement.bind(first, sample.timestamp);
        statement.bind(first + 1, sample.host_id);
        statement.bind(first + 2, sample.players);
    }

    void record(const Sample& sample) {
        ++added_;
        Totals& totals = *sample.totals;
        ++totals.samples;
        if (sample.players < 0) {
            ++totals.failed;
        } else if (sample.players == 0 && sample.timestamp < legacy_zero_cutoff) {
            ++totals.ambiguous;
        } else {
            totals.sum += sample.players;
            totals.low = std::min(totals.low, sample.players);
            totals.high = std::max(totals.high, sample.players);
        }
    }

    void insert_pending() {
        if (size_ == chunk_size) {
            for (int i = 0; i < size_; ++i)
                bind_sample(add_chunk_, 3 * i + 1, pending_[i]);
            if (add_chunk_.insert_unique()) {
                for (const auto& sample : pending_) record(sample);
                size_ = 0;
                return;
            }
            // ABORT rolls back the whole statement. Replay to count only new rows.
        }
        for (int i = 0; i < size_; ++i) {
            const Sample& sample = pending_[i];
            bind_sample(add_sample_, 1, sample);
            add_sample_.run();
            if (sqlite3_changes(db_) != 0) record(sample);
        }
        size_ = 0;
    }

    Host& find_host(std::string_view host) {
        const auto cached = hosts_.find(host);
        if (cached != hosts_.end()) return cached->second;
        if (host.empty() || host.size() > 255 ||
            host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_:[]") !=
                std::string_view::npos) {
            throw std::runtime_error("Invalid hostname: " + std::string(host));
        }
        add_host_.bind(1, host);
        add_host_.run();
        Statement find(db_, "SELECT id FROM hosts WHERE name=?");
        find.bind(1, host);
        if (!find.next()) throw std::runtime_error("Could not look up host");
        return hosts_.emplace(host, Host{find.integer(0)}).first->second;
    }

    sqlite3* db_;
    Statement add_host_;
    Statement add_chunk_;
    Statement add_sample_;
    std::unordered_map<std::string, Host, HostHash, std::equal_to<>> hosts_;
    std::map<std::pair<Integer, Integer>, Totals> totals_;
    std::array<Sample, chunk_size> pending_;
    int size_ = 0;
    Integer added_ = 0;
};

Integer parse_integer(std::string_view field) {
    Integer value = 0;
    const auto result = std::from_chars(field.data(), field.data() + field.size(), value);
    if (result.ec != std::errc{} || result.ptr != field.data() + field.size())
        throw std::runtime_error("Expected integer: " + std::string(field));
    return value;
}

// MineTrack CSV fields are integer, hostname and nullable integer.
std::array<std::string_view, 3> csv_row(std::string_view line) {
    std::array<std::string_view, 3> fields;
    for (auto& field : fields) {
        const auto comma = line.find(',');
        field = line.substr(0, comma);
        const bool last = &field == &fields.back();
        if (last != (comma == std::string_view::npos))
            throw std::runtime_error("Expected three CSV columns");
        if (!last) line.remove_prefix(comma + 1);
        if (field.size() >= 2 && field.front() == '"' && field.back() == '"') {
            field.remove_prefix(1);
            field.remove_suffix(1);
        }
    }
    return fields;
}

void trim_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
}

void print_progress(const std::string& source, const Progress& state) {
    std::cerr << source << "  scanned=" << state.scanned << " added=" << state.added
              << (state.complete ? " complete\n" : " checkpoint\n");
}

void csv_field(std::ostream& out, std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
        out << value;
        return;
    }
    out << '"';
    for (const char ch : value) {
        if (ch == '"') out << '"';
        out << ch;
    }
    out << '"';
}

void write_result(Statement& rows, bool csv, std::ostream& out) {
    const auto write_row = [&](bool header) {
        for (int i = 0; i < rows.columns(); ++i) {
            const std::string_view value = header ? rows.name(i) : rows.string(i);
            if (i) out << (csv ? "," : "  ");
            if (csv) {
                csv_field(out, value);
            } else if (i + 1 == rows.columns()) {
                out << value;
            } else {
                const auto name = rows.name(i);
                const std::size_t minimum = name == "host" ? 32 : name == "timestamp_ms" ? 15 : 10;
                const auto width = std::max({name.size(), value.size(), minimum});
                out << std::left << std::setw(static_cast<int>(width)) << value;
            }
        }
        out << (csv ? "\r\n" : "\n");
    };
    write_row(true);
    while (out && rows.next()) write_row(false);
    out.flush();
    if (!out) throw std::runtime_error("Could not write report");
}

void visit_raw_means(sqlite3* db, Integer host, Integer from, Integer to,
                     const Interval& base, bool reverse, Integer maximum,
                     const std::function<void(const MeanPoint&)>& consume,
                     const Interval* limit_period = nullptr) {
    if (from >= to) return;
    const std::string sql = R"SQL(
        SELECT timestamp_ms,players FROM samples
        WHERE timestamp_ms>=?1 AND timestamp_ms<?2 AND host_id=?3 AND players>0
        ORDER BY timestamp_ms )SQL" + std::string(reverse ? "DESC,host_id DESC,players DESC" : ",host_id,players");
    Statement rows(db, sql.c_str());
    rows.bind(1, from);
    rows.bind(2, to);
    rows.bind(3, host);
    Integer emitted = 0;
    std::optional<Integer> last_limit_bucket;
    MeanPoint point{};
    Integer bucket = -1;
    long double sum = 0;
    const auto emit = [&](const MeanPoint& value) {
        if (limit_period) {
            const Integer key = limit_period->bounds(value.begin).first;
            if (!last_limit_bucket || key != *last_limit_bucket) {
                if (maximum != 0 && emitted == maximum) return false;
                last_limit_bucket = key;
                ++emitted;
            }
        } else {
            ++emitted;
        }
        consume(value);
        return maximum == 0 || limit_period || emitted < maximum;
    };
    const auto flush = [&] {
        if (point.samples == 0) return true;
        point.players = static_cast<double>(sum / point.samples);
        const bool more = emit(point);
        point.samples = 0;
        sum = 0;
        return more;
    };
    while (rows.next()) {
        const Integer timestamp = rows.integer(0);
        if (base.unit == Interval::Unit::raw) {
            if (!emit({timestamp, timestamp + 1, static_cast<double>(rows.integer(1)), 1, timestamp, timestamp}))
                return;
            continue;
        }
        const auto [begin, end] = base.bounds(timestamp);
        if (point.samples != 0 && bucket != begin && !flush()) return;
        if (point.samples == 0) {
            bucket = begin;
            point = {std::max(from, begin), std::min(to, end), 0, 0, timestamp, timestamp};
        }
        ++point.samples;
        sum += rows.integer(1);
        point.first_sample = std::min(point.first_sample, timestamp);
        point.last_sample = std::max(point.last_sample, timestamp);
    }
    flush();
}

} // namespace

Database::Database(const fs::path& path, bool writable) {
    auto connection = connect(path, writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                             : SQLITE_OPEN_READONLY);
    sqlite3* db = connection.get();
    if (writable) {
        Transaction transaction(db);
        if (scalar(db, "PRAGMA application_id") == 0 &&
            scalar(db, "PRAGMA user_version") == 0 &&
            scalar(db, "SELECT count(*) FROM sqlite_master") == 0) {
            exec(db, R"SQL(
                CREATE TABLE hosts(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
                CREATE TABLE samples(
                    timestamp_ms INTEGER NOT NULL, host_id INTEGER NOT NULL, players INTEGER NOT NULL,
                    PRIMARY KEY(timestamp_ms, host_id, players)
                ) WITHOUT ROWID;
                CREATE TABLE daily(
                    day INTEGER NOT NULL, host_id INTEGER NOT NULL, samples INTEGER NOT NULL,
                    failed INTEGER NOT NULL, ambiguous INTEGER NOT NULL, players_sum INTEGER NOT NULL,
                    low INTEGER NOT NULL, high INTEGER NOT NULL, PRIMARY KEY(day, host_id)
                ) WITHOUT ROWID;
                CREATE TABLE imports(
                    source TEXT PRIMARY KEY, cursor INTEGER NOT NULL, scanned INTEGER NOT NULL,
                    added INTEGER NOT NULL, complete INTEGER NOT NULL
                ) WITHOUT ROWID;
                PRAGMA user_version=1;
            )SQL");
            exec(db, ("PRAGMA application_id=" + std::to_string(application_id)).c_str());
        }
        transaction.commit();
    }
    if (scalar(db, "PRAGMA application_id") != application_id || scalar(db, "PRAGMA user_version") != 1)
        throw std::runtime_error("Expected a MineTrack database with schema version 1");
    if (writable) {
        exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA cache_size=-65536");
    }
    db_ = connection.release();
}

Database::~Database() { sqlite3_close(db_); }

bool Database::imported(std::string_view source) const { return progress(db_, source).complete; }

void Database::import_csv(const fs::path& path, const std::string& source) {
    Transaction transaction(db_);
    Progress state = progress(db_, source);
    if (state.complete) return;

    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("Missing CSV header in " + path_utf8(path));
    trim_cr(line);
    if (line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
    const auto header = csv_row(line);
    if (header != std::array<std::string_view, 3>{"timestamp", "ip", "playerCount"})
        throw std::runtime_error("Unexpected CSV header in " + path_utf8(path));

    Batch batch(db_);
    while (std::getline(in, line)) {
        ++state.scanned;
        trim_cr(line);
        try {
            const auto [timestamp, host, count] = csv_row(line);
            const bool missing = count.empty() || count == "NULL" || count == "null";
            const auto players = missing ? std::nullopt : std::optional{parse_integer(count)};
            batch.add(parse_integer(timestamp), host, players);
        } catch (const std::exception& error) {
            throw std::runtime_error(path_utf8(path) + " line " + std::to_string(state.scanned + 1) +
                                     ": " + error.what());
        }
    }
    if (in.bad() || !in.eof()) throw std::runtime_error("Error reading " + path_utf8(path));
    state.added += batch.flush();
    state.complete = true;
    save_progress(db_, source, state);
    transaction.commit();
    print_progress(source, state);
}

void Database::import_sqlite(const fs::path& path, const std::string& source) {
    if (imported(source)) return;
    const auto input = connect(path, SQLITE_OPEN_READONLY);
    // Allow mapped reads of the large archive up to the SQLite build's limit.
    exec(input.get(), "PRAGMA mmap_size=68719476736; BEGIN; PRAGMA query_only=ON; PRAGMA trusted_schema=OFF");

    constexpr Integer batch_size = 500'000;
    for (;;) {
        Transaction transaction(db_);
        Progress state = progress(db_, source);
        if (state.complete) return;
        Batch batch(db_);
        Statement rows(input.get(), R"SQL(
            SELECT rowid, timestamp, ip, playerCount FROM pings
            WHERE rowid>? ORDER BY rowid LIMIT ?
        )SQL");
        rows.bind(1, state.rowid);
        rows.bind(2, batch_size);
        Integer count = 0;
        while (rows.next()) {
            state.rowid = rows.integer(0);
            const bool missing = rows.type(3) == SQLITE_NULL;
            if (rows.type(1) != SQLITE_INTEGER || rows.type(2) != SQLITE_TEXT ||
                (!missing && rows.type(3) != SQLITE_INTEGER)) {
                throw std::runtime_error("Invalid pings row at rowid " + std::to_string(state.rowid));
            }
            const auto players = missing ? std::nullopt : std::optional{rows.integer(3)};
            try {
                batch.add(rows.integer(1), rows.string(2), players);
            } catch (const std::exception& error) {
                throw std::runtime_error("pings rowid " + std::to_string(state.rowid) + ": " + error.what());
            }
            ++count;
        }
        state.scanned += count;
        state.complete = count < batch_size;
        state.added += batch.flush();
        save_progress(db_, source, state);
        transaction.commit();
        if (state.complete || state.scanned % (10 * batch_size) == 0) print_progress(source, state);
        if (state.complete) return;
    }
}

void Database::report(std::string_view command, Day from, Day to, std::string_view host,
                      std::string_view period, Integer limit, std::string_view format,
                      const std::optional<fs::path>& output) const {
    const Integer first = from.time_since_epoch().count();
    const Integer last = to.time_since_epoch().count();
    std::string sql;
    if (command == "--list-hosts") {
        sql = "SELECT name AS host FROM hosts ORDER BY name COLLATE BINARY";
    } else if (command == "sources") {
        sql = "SELECT scanned, added, CASE complete WHEN 1 THEN 'complete' ELSE 'partial' END"
              " AS status, source FROM imports ORDER BY source";
    } else if (command == "raw") {
        sql = R"SQL(
            SELECT timestamp_ms, name AS host,
                   CASE WHEN players<-1 THEN players+1 ELSE NULLIF(players, -1) END AS players,
                   CASE WHEN players=-1 THEN 'failed'
                        WHEN players<-1 THEN 'invalid'
                        WHEN players=0 AND timestamp_ms<?5 THEN 'ambiguous'
                        ELSE 'ok' END AS status
            FROM samples JOIN hosts ON hosts.id=samples.host_id
            WHERE timestamp_ms>=?1 AND timestamp_ms<?2 AND (?3='' OR name=?3)
            ORDER BY timestamp_ms, host_id, samples.players LIMIT ?4
        )SQL";
    } else if (command == "coverage") {
        sql = R"SQL(
            WITH RECURSIVE dates(day) AS (
                SELECT ?1 UNION ALL SELECT day+1 FROM dates WHERE day<?2
            ), counts AS (
                SELECT day, count(*) AS hosts, sum(samples) AS samples
                FROM daily JOIN hosts ON hosts.id=daily.host_id
                WHERE day BETWEEN ?1 AND ?2 AND (?3='' OR name=?3)
                GROUP BY day
            )
            SELECT date(dates.day*86400, 'unixepoch') AS date,
                   coalesce(counts.hosts, 0) AS hosts, coalesce(counts.samples, 0) AS samples
            FROM dates LEFT JOIN counts USING(day) ORDER BY dates.day
        )SQL";
    } else {
        const bool trend = command == "trend";
        const char* date_format = "%Y-%m-%d";
        if (period == "month") date_format = "%Y-%m";
        if (period == "year") date_format = "%Y";
        sql = "WITH totals AS (SELECT ";
        if (trend) sql += "strftime('" + std::string(date_format) + "',day*86400,'unixepoch') AS period, ";
        sql += R"SQL(
            name AS host, sum(samples) AS samples, sum(failed) AS failed, sum(ambiguous) AS ambiguous,
            sum(samples-failed-ambiguous) AS responding, sum(players_sum) AS players_sum,
            min(low) AS low, max(high) AS high
            FROM daily JOIN hosts ON hosts.id=daily.host_id
            WHERE day BETWEEN ?1 AND ?2 AND (?3='' OR name=?3)
        )SQL";
        sql += trend ? "GROUP BY period, host) SELECT period, " : "GROUP BY host) SELECT ";
        sql += R"SQL(
            host, samples, responding, failed, ambiguous,
            CASE WHEN responding>0 THEN low END AS min_players,
            CASE WHEN responding>0 THEN round(1.0*players_sum/responding, 2) END AS avg_players,
            CASE WHEN responding>0 THEN high END AS peak_players,
            CASE WHEN responding+failed>0 THEN round(100.0*responding/(responding+failed), 2)
            END AS response_pct FROM totals
        )SQL";
        sql += trend ? "ORDER BY period, host" :
            "ORDER BY 1.0*players_sum/NULLIF(responding,0) DESC, peak_players DESC, host LIMIT ?4";
    }

    Statement query(db_, sql.c_str());
    if (command != "sources" && command != "--list-hosts") {
        const bool raw = command == "raw";
        query.bind(1, raw ? first * day_ms : first);
        query.bind(2, raw ? (last + 1) * day_ms : last);
        query.bind(3, host);
        if (command == "rank" || raw) query.bind(4, limit);
        if (raw) query.bind(5, legacy_zero_cutoff);
    }

    if (output) {
        std::ofstream file(*output, std::ios::binary);
        if (!file) throw std::runtime_error("Could not open report " + path_utf8(*output));
        write_result(query, format == "csv", file);
        file.close();
        if (!file) throw std::runtime_error("Could not close report " + path_utf8(*output));
    } else {
        write_result(query, format == "csv", std::cout);
    }
}

std::vector<Candle> Database::candles(std::string_view host, Integer from, Integer to,
                                      const CandleOptions& options) const {
    Transaction snapshot(db_, false);
    Statement find(db_, "SELECT id FROM hosts WHERE name=?");
    find.bind(1, host);
    if (!find.next()) throw std::runtime_error("Unknown hostname: " + std::string(host));
    const Integer host_id = find.integer(0);
    const bool calendar_period = options.period.unit == Interval::Unit::week ||
        options.period.unit == Interval::Unit::month || options.period.unit == Interval::Unit::year;
    if (options.method == "none" && (calendar_period ||
        (options.period.milliseconds() >= day_ms && options.period.milliseconds() % day_ms == 0))) {
        // Exact raw OHLC: extrema/counts come from existing daily summaries;
        // only clipped days, zero minima and endpoint samples require raw reads.
        std::vector<Candle> result;
        Statement days(db_, "SELECT day,samples-failed-ambiguous,low,high FROM daily WHERE host_id=?1 AND day BETWEEN ?2 AND ?3 ORDER BY day");
        days.bind(1, host_id); days.bind(2, from / day_ms); days.bind(3, (to-1)/day_ms);
        Statement exact(db_, "SELECT count(*),min(players),max(players) FROM samples WHERE timestamp_ms>=?1 AND timestamp_ms<?2 AND host_id=?3 AND players>0");
        while (days.next()) {
            const Integer start = std::max(from, days.integer(0)*day_ms);
            const Integer stop = std::min(to, (days.integer(0)+1)*day_ms);
            Integer count=days.integer(1), low=days.integer(2), high=days.integer(3);
            if (low == 0 || start % day_ms || stop % day_ms) {
                exact.bind(1,start); exact.bind(2,stop); exact.bind(3,host_id);
                if (exact.next()) { count=exact.integer(0); low=exact.integer(1); high=exact.integer(2); }
                exact.reset();
            }
            if (count<=0) continue;
            const auto [begin,end]=options.period.bounds(start);
            if (result.empty() || result.back().begin!=begin)
                result.push_back({options.period.label(begin),begin,end,start,stop-1,0,double(high),double(low),0,0,0,0});
            auto& c=result.back();
            c.high=std::max(c.high,double(high)); c.low=std::min(c.low,double(low));
            ++c.days; c.samples+=count; c.points+=count;
        }
        std::optional<double> previous;
        Statement prior(db_, "SELECT players FROM samples WHERE timestamp_ms<?1 AND host_id=?2 AND players>0 ORDER BY timestamp_ms DESC,host_id DESC,players DESC LIMIT 1");
        prior.bind(1,options.period.bounds(from).first); prior.bind(2,host_id);
        if (prior.next()) previous=double(prior.integer(0));
        for (std::size_t i=0;i<result.size();++i) {
            auto& c=result[i];
            const Integer start=std::max(from,c.begin), stop=std::min(to,c.end);
            for (bool reverse : {false,true}) {
                const std::string order=reverse ? " DESC,host_id DESC,players DESC" : ",host_id,players";
                Statement edge(db_,("SELECT timestamp_ms,players FROM samples WHERE timestamp_ms>=?1 AND timestamp_ms<?2 AND host_id=?3 AND players>0 ORDER BY timestamp_ms"+order+" LIMIT 1").c_str());
                edge.bind(1,start);edge.bind(2,stop);edge.bind(3,host_id);
                if (!edge.next()) throw std::runtime_error("Missing raw OHLC endpoint");
                if (reverse) {c.last=edge.integer(0);c.close=double(edge.integer(1));}
                else {c.first=edge.integer(0);c.open=previous.value_or(double(edge.integer(1)));}
            }
            c.high=std::max(c.high,c.open);c.low=std::min(c.low,c.open);previous=c.close;
            if ((i+1)%100==0) std::cerr << "Raw OHLC: " << i+1 << '/' << result.size() << " periods\n";
        }
        if(options.limit>0 && result.size()>static_cast<std::size_t>(options.limit))
            result.erase(result.begin(),result.end()-options.limit);
        snapshot.commit();
        return result;
    }
    CandleBuilder builder(options, from);
    const auto consume = [&](const MeanPoint& point) { builder.add(point); };
    if (options.base.unit != Interval::Unit::day || options.base.count != 1) {
        {
            // A bounded history keeps short intraday queries from scanning the whole archive.
            // KAMA needs a longer warm-up because its slowest alpha is (2/31)^2.
            std::vector<MeanPoint> warmup;
            long double warmup_sum = 0;
            const auto collect = [&](const MeanPoint& point) {
                if (options.method != "index") {
                    warmup.push_back(point);
                    return;
                }
                const auto key = options.period.bounds(point.begin).first;
                if (warmup.empty() || options.period.bounds(warmup.back().begin).first != key) {
                    if (!warmup.empty())
                        warmup.back().players = static_cast<double>(warmup_sum / warmup.back().samples);
                    warmup.push_back(point);
                    warmup_sum = static_cast<long double>(point.players) * point.samples;
                } else {
                    auto& group = warmup.back();
                    warmup_sum += static_cast<long double>(point.players) * point.samples;
                    group.samples += point.samples;
                    group.begin = std::min(group.begin, point.begin);
                    group.end = std::max(group.end, point.end);
                    group.first_sample = std::min(group.first_sample, point.first_sample);
                    group.last_sample = std::max(group.last_sample, point.last_sample);
                }
            };
            const Integer history = options.method == "mean" || options.method == "none" ? 1 :
                options.method == "index" ? options.span * 10 :
                options.span * 20 + (options.method == "kama" ? 5000 : 0);
            visit_raw_means(db_, host_id, 0, options.period.bounds(from).first, options.base, true,
                            history, collect,
                            options.method == "index" ? &options.period : nullptr);
            if (options.method == "index" && !warmup.empty())
                warmup.back().players = static_cast<double>(warmup_sum / warmup.back().samples);
            for (auto it = warmup.rbegin(); it != warmup.rend(); ++it) builder.add(*it);
        }
        visit_raw_means(db_, host_id, from, to, options.base, false, 0, consume);
        snapshot.commit();
        return builder.finish();
    }
    Statement days(db_, R"SQL(
        SELECT day, samples-failed-ambiguous, players_sum, low
        FROM daily WHERE host_id=?1 AND day BETWEEN ?2 AND ?3 ORDER BY day
    )SQL");
    days.bind(1, host_id);
    days.bind(2, 0);
    days.bind(3, (to - 1) / day_ms);
    Statement zeros(db_, R"SQL(
        SELECT count(*) FROM samples
        WHERE timestamp_ms>=?1 AND timestamp_ms<?2 AND host_id=?3 AND players=0
    )SQL");
    while (days.next()) {
        const Integer day = days.integer(0);
        const Integer day_begin = day * day_ms;
        const Integer day_end = (day + 1) * day_ms;
        const Integer begin = day_begin < from && day_end > from ? from : day_begin;
        const Integer end = std::min(to, day_end);
        if (begin != day_begin || end != day_end) {
            visit_raw_means(db_, host_id, begin, end, options.base, false, 0, consume);
            continue;
        }
        Integer count = days.integer(1);
        if (days.integer(3) == 0) {
            // Later successful zero counts are included in the existing rollup.
            // Only days whose recorded minimum is zero need a raw-data lookup.
            zeros.bind(1, day * day_ms);
            zeros.bind(2, (day + 1) * day_ms);
            zeros.bind(3, host_id);
            if (!zeros.next()) throw std::runtime_error("Could not count zero samples");
            count -= zeros.integer(0);
            if (zeros.next()) throw std::runtime_error("Unexpected zero-count row");
            zeros.reset();
        }
        if (count > 0)
            builder.add({begin, end, static_cast<double>(days.integer(2)) / static_cast<double>(count),
                         count, begin, end - 1});
    }
    snapshot.commit();
    return builder.finish();
}

std::string local_source(const fs::path& path) {
    return "file:" + path_utf8(fs::canonical(path)) + "|" +
        std::to_string(fs::file_size(path)) + "|" +
        std::to_string(fs::last_write_time(path).time_since_epoch().count());
}
