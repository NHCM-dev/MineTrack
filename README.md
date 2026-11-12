# MineTrack

C++20 CLI for downloading and analyzing MineTrack Java server history.
Stores all tracked servers in SQLite. No Python required.

## Build

Requires CMake 3.20+, a C++20 compiler, and SQLite 3.26+ development libraries.
Windows builds can use MSYS2 UCRT64 with its `bin` directory on PATH.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Sync also requires `curl` and ZIP64-capable BSD tar (`tar.exe` on Windows,
`bsdtar` on Linux).

## Usage

Examples below run from `work` on Windows. Use `./build/minetrack` on Linux/macOS
and adjust database paths as needed.

```powershell
# Download all available Java history; rerun to resume or update
.\minetrack.exe sync --db java.sqlite

# List known hosts or export them
.\minetrack.exe --list-hosts --db java.sqlite
.\minetrack.exe --list-hosts --db java.sqlite --output hosts.csv

# Auto-name a PNG and matching OHLC CSV
.\minetrack.exe candles --db java.sqlite --host mc.hypixel.net --period day --with-csv --chart-dir charts

# Export all 12 methods to one directory
.\minetrack.exe candles --db java.sqlite --host mc.hypixel.net --period week --from 2026-09-01 --method all --with-csv --chart-dir charts-all

# Generate 13 yearly/monthly/weekly/daily history and recent-window charts
.\minetrack.exe candles --db java.sqlite --host mc.hypixel.net --chart-set --with-csv --chart-dir chart-set

.\minetrack.exe methods
.\minetrack.exe --help
```

Periods: `day`, `week`, `month`, `year`, or intervals such as `15m`, `4h`, `2d`.
Methods: `index` (default), `none`, `mean`, `sma`, `ema`, `wma`, `rma`, `dema`,
`tema`, `hma`, `kama`, `median`.

Charts use green for rises and red for falls. Each open equals the previous valid
close. Zero, negative, and failed samples are excluded from charts; raw data stays
in the database. All times are UTC. Filtered OHLC values are derived indicators.

Use `--format csv` for CSV alone, `--output name.png` for an explicit image name,
and `--font path.ttf` to override the system font. Automatic names preserve existing
files by adding a numeric suffix.

Data: [MineTrack Data](https://data.minetrack.me/) (CC0).
Bundled graphics code: [stb](third_party/stb/LICENSE).
