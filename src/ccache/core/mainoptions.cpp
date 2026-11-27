// Copyright (C) 2021-2026 Joel Rosdahl and other contributors
//
// See doc/authors.adoc for a complete list of contributors.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 51
// Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "mainoptions.hpp"

#include <ccache/ccache.hpp>
#include <ccache/config.hpp>
#include <ccache/core/cacheentry.hpp>
#include <ccache/core/exceptions.hpp>
#include <ccache/core/filerecompressor.hpp>
#include <ccache/core/manifest.hpp>
#include <ccache/core/result.hpp>
#include <ccache/core/resultextractor.hpp>
#include <ccache/core/resultinspector.hpp>
#include <ccache/core/statistics.hpp>
#include <ccache/core/statslog.hpp>
#include <ccache/hash.hpp>
#include <ccache/inodecache.hpp>
#include <ccache/progressbar.hpp>
#include <ccache/storage/local/localstorage.hpp>
#include <ccache/storage/storage.hpp>
#include <ccache/util/assertions.hpp>
#include <ccache/util/cpu.hpp>
#include <ccache/util/environment.hpp>
#include <ccache/util/expected.hpp>
#include <ccache/util/fd.hpp>
#include <ccache/util/file.hpp>
#include <ccache/util/filestream.hpp>
#include <ccache/util/filesystem.hpp>
#include <ccache/util/format.hpp>
#include <ccache/util/logging.hpp>
#include <ccache/util/path.hpp>
#include <ccache/util/string.hpp>
#include <ccache/util/temporaryfile.hpp>
#include <ccache/util/texttable.hpp>
#include <ccache/util/threadpool.hpp>
#include <ccache/util/time.hpp>
#include <ccache/util/umaskscope.hpp>
#include <ccache/util/xxh3_128.hpp>

#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <rfl/cli.hpp>

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif

namespace fs = util::filesystem;

using util::DirEntry;

namespace core {

constexpr const char VERSION_TEXT[] =
  R"({0} version {1}
Features: {2}

Copyright (C) 2002-2007 Andrew Tridgell
Copyright (C) 2009-2026 Joel Rosdahl and other contributors

See <https://ccache.dev/credits.html> for a complete list of contributors.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 3 of the License, or (at your option) any later
version.
)";

constexpr const char USAGE_TEXT[] =
  R"(Usage:
    {0} [ccache options]
    {0} [KEY=VALUE ...] compiler [compiler options]
    compiler [compiler options]

    The first form takes options described below. The second form invokes the
    compiler, optionally using configuration options from KEY=VALUE arguments.
    In the third form, ccache is masquerading as the compiler.

Common options:
    -c, --cleanup              delete not recently used files and recalculate
                               size counters (normally not needed as this is
                               done automatically)
    -C, --clear                clear the cache completely (except configuration)
        --config-path PATH     operate on configuration file PATH instead of the
                               default
    -d, --dir PATH             operate on cache directory PATH instead of the
                               default
        --dry-run              do not perform any write operations
        --evict-namespace NAMESPACE
                               remove files created in namespace NAMESPACE
        --evict-older-than AGE remove files used less recently than AGE
                               (unsigned integer with a d (days) or s (seconds)
                               suffix)
    -F, --max-files NUM        set maximum number of files in cache to NUM (use
                               0 for no limit)
    -M, --max-size SIZE        set maximum size of cache to SIZE (use 0 for no
                               limit); available suffixes: kB, MB, GB, TB
                               (decimal) and KiB, MiB, GiB, TiB (binary);
                               default suffix: GiB
    -X, --recompress LEVEL     recompress the cache to level LEVEL (integer or
                               "uncompressed")
    -o, --set-config KEY=VALUE set configuration option KEY to value VALUE in the
                               configuration file
    -x, --show-compression     show compression statistics
    -p, --show-config          show current configuration options in
                               human-readable format
        --show-log-stats       print statistics counters from the stats log
                               in human-readable format
    -s, --show-stats           show summary of configuration and statistics
                               counters in human-readable format (use
                               -v/--verbose once or twice for more details)
        --threads THREADS      use up to THREADS threads for threaded operations;
                               default: number of CPUs
    -v, --verbose              increase verbosity
    -z, --zero-stats           zero statistics counters

    -h, --help                 print this help text
    -V, --version              print version and copyright information

Options for remote file-based storage:
        --trim-dir PATH        remove not recently used files from directory
                               PATH until it is at most the size specified by
                               --trim-max-size (note: don't use this option to
                               trim the local cache)
        --trim-log PATH        write log of removed files to PATH
        --trim-marker PATH     skip --trim-recompress for files older than PATH
        --trim-max-size SIZE   specify the maximum size for --trim-dir (use 0 for
                               no limit); available suffixes: kB, MB, GB, TB
                               (decimal) and KiB, MiB, GiB, TiB (binary);
                               default suffix: GiB
        --trim-method METHOD   specify the method (atime or mtime) for
                               --trim-dir; default: atime
        --trim-recompress LEVEL
                               recompress to level LEVEL (integer or
                               "uncompressed")

Options for scripting or debugging:
        --checksum-file PATH   print the checksum (128 bit XXH3) of the file at
                               PATH
        --extract-result PATH  extract file data stored in result file at PATH
                               to the current working directory
        --format FORMAT        specify format for --print-log-stats and
                               --print-stats (tab, json); default: tab
    -k, --get-config KEY       print the value of configuration key KEY
        --hash-file PATH       print the hash (160 bit BLAKE3) of the file at
                               PATH
        --inspect PATH         print result/manifest file at PATH in
                               human-readable format
        --stop-storage-helpers stop running storage helper(s), if any
        --print-log-stats      print statistics counter IDs and corresponding
                               values from the stats log in machine-parsable
                               format
        --print-stats          print statistics counter IDs and corresponding
                               values in machine-parsable format
        --print-version        print version only

See also the manual on <https://ccache.dev/documentation.html>.
)";

static void
configuration_printer(const std::string& key,
                      const std::string& value,
                      const std::string& origin)
{
  PRINT(stdout, "({}) {} = {}\n", origin, key, value);
}

static tl::expected<util::Bytes, std::string>
read_from_path_or_stdin(const fs::path& path)
{
  if (path == "-") {
    return util::read_fd(STDIN_FILENO).transform_error([&](auto error) {
      return FMT("Failed to read from stdin: {}", error);
    });
  } else {
    return util::read_file<util::Bytes>(path).transform_error(
      [&](auto error) { return FMT("Failed to read {}: {}", path, error); });
  }
}

static int
inspect_path(const fs::path& path)
{
  std::optional<util::DirEntry> orig_dir_entry;
  if (path != "-") {
    orig_dir_entry = util::DirEntry(path);
    orig_dir_entry->refresh();
  }
  const auto cache_entry_data = read_from_path_or_stdin(path);
  if (orig_dir_entry) {
    util::set_timestamps(
      path, orig_dir_entry->mtime(), orig_dir_entry->atime());
  }

  if (!cache_entry_data) {
    PRINT(stderr, "Error: {}\n", cache_entry_data.error());
    return EXIT_FAILURE;
  }

  core::CacheEntry cache_entry(*cache_entry_data);
  std::ignore = fputs(cache_entry.header().inspect().c_str(), stdout);

  const auto payload = cache_entry.payload();

  switch (cache_entry.header().entry_type) {
  case core::CacheEntryType::manifest: {
    core::Manifest manifest;
    manifest.read(payload);
    manifest.inspect(stdout);
    break;
  }
  case core::CacheEntryType::result:
    result::Deserializer result_deserializer(payload);
    ResultInspector result_inspector(stdout);
    result_deserializer.visit(result_inspector);
    break;
  }

  cache_entry.verify_checksum();

  return EXIT_SUCCESS;
}

static void
print_compression_statistics(const Config& config,
                             const storage::local::CompressionStatistics& cs)
{
  const double ratio = cs.actual_size > 0
                         ? static_cast<double>(cs.content_size)
                             / static_cast<double>(cs.actual_size)
                         : 0.0;
  const double savings = ratio > 0.0 ? 100.0 - (100.0 / ratio) : 0.0;

  auto human_readable = [&](uint64_t size) {
    return util::format_human_readable_size(size,
                                            config.size_unit_prefix_type());
  };

  const auto [total_data_quantity, total_data_unit] = util::split_once(
    human_readable(cs.actual_size + cs.incompressible_size), ' ');
  ASSERT(total_data_unit);
  const auto [compressed_data_quantity, compressed_data_unit] =
    util::split_once(human_readable(cs.actual_size), ' ');
  ASSERT(compressed_data_unit);
  const auto [original_data_quantity, original_data_unit] =
    util::split_once(human_readable(cs.content_size), ' ');
  ASSERT(original_data_unit);
  const auto [incompressible_data_quantity, incompressible_data_unit] =
    util::split_once(human_readable(cs.incompressible_size), ' ');
  ASSERT(incompressible_data_unit);

  using C = util::TextTable::Cell;
  util::TextTable table;

  table.add_row({
    "Total data:",
    C(total_data_quantity).right_align(),
    *total_data_unit,
  });
  table.add_row({
    "Compressed data:",
    C(compressed_data_quantity).right_align(),
    *compressed_data_unit,
    FMT("({:.1f}% of original size)", 100.0 - savings),
  });
  table.add_row({
    "  Original size:",
    C(original_data_quantity).right_align(),
    *original_data_unit,
  });
  table.add_row({
    "  Compression ratio:",
    C(FMT("{:.3f}", ratio)).right_align(),
    "x",
    FMT("({:.1f}% space savings)", savings),
  });
  table.add_row({
    "Incompressible data:",
    C(incompressible_data_quantity).right_align(),
    *incompressible_data_unit,
  });

  PRINT(stdout, "{}", table.render());
}

static std::string
recompress_level_to_string(const std::optional<int8_t>& level)
{
  return level ? std::to_string(static_cast<int>(*level)) : "uncompressed";
}

struct TrimDirOptions
{
  core::DryRun dry_run;
  std::string dir;
  std::optional<fs::path> marker_path;
  std::optional<fs::path> trim_log_path;
  uint64_t max_size;
  util::SizeUnitPrefixType suffix_type;
  bool lru_mtime;
  std::optional<std::optional<int8_t>> recompress_level;
  uint32_t threads;
};

static void
trim_dir(const TrimDirOptions& options)
{
  const auto dry_run = options.dry_run;
  const auto& dir = options.dir;
  const auto& trim_marker_path = options.marker_path;
  const auto& trim_log_path = options.trim_log_path;
  const auto trim_max_size = options.max_size;
  const auto suffix_type = options.suffix_type;
  const auto trim_lru_mtime = options.lru_mtime;
  const auto& recompress_level = options.recompress_level;
  const auto threads = options.threads;

  ASSERT(dry_run == DryRun::no
         || recompress_level == std::nullopt); // Verified by caller

  // Note: run_start_time is captured before traversal and used as the new
  // marker mtime (any file written concurrently with this run will have a newer
  // mtime and be picked up by the next run).
  util::TimePoint recompress_cutoff;
  util::TimePoint run_start_time;
  if (recompress_level && trim_marker_path) {
    run_start_time = util::now();
    DirEntry marker_entry(*trim_marker_path);
    if (marker_entry) {
      const auto content = util::read_file<std::string>(*trim_marker_path);
      const auto previous_level =
        content ? util::strip_whitespace(*content) : std::string_view();
      if (previous_level == recompress_level_to_string(*recompress_level)) {
        recompress_cutoff = marker_entry.mtime();
      }
    }
  }

  std::vector<DirEntry> files;
  uint64_t initial_size = 0;

  util::throw_on_error<core::Error>(
    util::traverse_directory(dir, [&](const auto& de) {
      if (de.is_directory() || util::TemporaryFile::is_tmp_file(de.path())) {
        return;
      }
      if (!de) {
        // Probably some race, ignore.
        return;
      }
      // Don't recompress or evict the marker file.
      if (trim_marker_path
          && de.path().filename() == trim_marker_path->filename()
          && fs::equivalent(de.path(), *trim_marker_path)) {
        return;
      }
      initial_size += de.size_on_disk();
      const auto name = de.path().filename();
      if (name == "ccache.conf" || name == "stats") {
        throw Fatal(
          FMT("this looks like a local cache directory (found {})", de.path()));
      }
      files.emplace_back(de);
    }));

  std::sort(files.begin(), files.end(), [&](const auto& f1, const auto& f2) {
    return trim_lru_mtime ? f1.mtime() < f2.mtime() : f1.atime() < f2.atime();
  });

  int64_t recompression_diff = 0;

  if (recompress_level) {
    const size_t queue_size = std::max<size_t>(10, 2 * threads);
    util::ThreadPool thread_pool(threads, queue_size);
    core::FileRecompressor recompressor;

    std::atomic<uint64_t> incompressible_size = 0;
    size_t skipped_files = 0;
    for (auto& file : files) {
      if (file.mtime() < recompress_cutoff) {
        ++skipped_files;
        continue;
      }
      thread_pool.enqueue_detach([&] {
        try {
          auto new_stat = recompressor.recompress(
            file, *recompress_level, core::FileRecompressor::KeepAtime::yes);
          auto old_size = file.size();
          auto new_size = new_stat.size();
          if (new_size != old_size) {
            LOG("Recompressed {} from {} to {} bytes",
                file.path(),
                old_size,
                new_size);
            file = std::move(new_stat); // Remember for sum calculation later
          }
        } catch (core::Error& e) {
          LOG("Error when recompressing {}: {}", file.path(), e.what());
          incompressible_size += file.size_on_disk();
        }
      });
    }

    thread_pool.shut_down();
    recompression_diff = recompressor.new_size() - recompressor.old_size();
    if (skipped_files == files.size()) {
      PRINT(stdout, "No new cache entries to recompress\n");
    } else {
      PRINT(stdout,
            "Recompressed {} to {} ({})\n",
            util::format_human_readable_size(
              incompressible_size + recompressor.old_size(), suffix_type),
            util::format_human_readable_size(
              incompressible_size + recompressor.new_size(), suffix_type),
            util::format_human_readable_diff(recompression_diff, suffix_type));
    }
    if (trim_marker_path) {
      util::throw_on_error<core::Error>(util::write_file(
        *trim_marker_path,
        FMT("{}\n", recompress_level_to_string(*recompress_level))));
      util::set_timestamps(*trim_marker_path, run_start_time);
    }
  }

  uint64_t size_after_recompression = initial_size + recompression_diff;
  uint64_t final_size = size_after_recompression;

  util::FileStream trim_log;
  if (trim_log_path && dry_run == DryRun::no) {
    trim_log.open(*trim_log_path, "a");
    if (!trim_log) {
      throw Fatal(FMT("failed to open {} for appending: {}",
                      *trim_log_path,
                      strerror(errno)));
    }
  }

  size_t removed_files = 0;
  if (trim_max_size > 0) {
    for (const auto& file : files) {
      if (final_size <= trim_max_size) {
        break;
      }
      if (dry_run == DryRun::yes || util::remove(file.path())) {
        ++removed_files;
        final_size -= file.size_on_disk();
        if (trim_log) {
          PRINT(
            *trim_log,
            "{{\"removed_at\":\"{}Z\",\"atime\":\"{}Z\",\"mtime\":\"{}Z\","
            "\"size\":{},\"path\":{}}}\n",
            util::format_iso8601_timestamp(util::now(), util::TimeZone::utc),
            util::format_iso8601_timestamp(file.atime(), util::TimeZone::utc),
            util::format_iso8601_timestamp(file.mtime(), util::TimeZone::utc),
            file.size(),
            util::format_json_string(util::pstr(file.path()).str()));
        }
      }
    }
  }

  PRINT(stdout,
        "Trimmed {} to {} ({}, {}{} file{})\n",
        util::format_human_readable_size(size_after_recompression, suffix_type),
        util::format_human_readable_size(final_size, suffix_type),
        util::format_human_readable_diff(final_size - size_after_recompression,
                                         suffix_type),
        removed_files == 0 ? "" : "-",
        removed_files,
        removed_files == 1 ? "" : "s");
}

std::optional<int8_t>
parse_compression_level(std::string_view level)
{
  if (level == "uncompressed") {
    return std::nullopt;
  } else {
    return util::value_or_throw<Error>(
      util::parse_signed(level, INT8_MIN, INT8_MAX, "compression level"));
  }
}

static std::string
get_version_text(const std::string_view ccache_name)
{
  auto features = storage::get_features();
  if (util::cpu_supports_avx2()) {
    features.emplace_back("avx2");
  }
  std::sort(features.begin(), features.end());
  return FMT(
    VERSION_TEXT, ccache_name, CCACHE_VERSION, util::join(features, " "));
}

std::string
get_usage_text(const std::string_view ccache_name)
{
  return FMT(USAGE_TEXT, ccache_name);
}

using rfl::cli::Flag;
using rfl::cli::Opt;
using namespace rfl::cli::attr;

struct MainOptions
{
  std::vector<std::string> checksum_file{};
  Flag<Short<"c">> cleanup{false};
  Flag<Short<"C">> clear{false};
  std::optional<std::string> config_path{};
  Opt<std::optional<std::string>, Short<"d">, Alias<"directory">> dir{};
  Flag<Excludes<"clear">,
       Excludes<"extract-result">,
       Excludes<"max-files">,
       Excludes<"max-size">,
       Excludes<"recompress">,
       Excludes<"set-config">,
       Excludes<"stop-storage-helpers">,
       Excludes<"trim-recompress">,
       Excludes<"zero-stats">>
    dry_run{false};
  std::optional<std::string> evict_namespace{};
  std::optional<std::string> evict_older_than{};
  std::optional<std::string> extract_result{};
  std::optional<std::string> format{};
  Opt<std::vector<std::string>, Short<"k">> get_config{};
  std::vector<std::string> hash_file{};
  Flag<Short<"h">> help{false};
  Opt<std::optional<std::string>, Alias<"dump-manifest">, Alias<"dump-result">>
    inspect{};
  Opt<std::optional<std::string>, Short<"F">> max_files{};
  Opt<std::optional<std::string>, Short<"M">> max_size{};
  bool print_log_stats = false;
  bool print_stats = false;
  bool print_version = false;
  Opt<std::optional<std::string>, Short<"X">> recompress{};
  Opt<std::vector<std::string>, Short<"o">> set_config{};
  Flag<Short<"x">> show_compression{false};
  Flag<Short<"p">> show_config{false};
  bool show_log_stats = false;
  Flag<Short<"s">> show_stats{false};
  bool stop_storage_helpers = false;
  Opt<std::optional<std::string>,
      Alias<"recompress-threads">,
      Alias<"trim-recompress-threads">>
    threads{};
  std::optional<std::string> trim_dir{};
  std::optional<std::string> trim_log{};
  std::optional<std::string> trim_marker{};
  std::optional<std::string> trim_max_size{};
  std::optional<std::string> trim_method{};
  std::optional<std::string> trim_recompress{};
  Opt<uint8_t, Short<"v">, Counter> verbose{uint8_t{0}};
  Flag<Short<"V">> version{false};
  Flag<Short<"z">> zero_stats{false};
  Opt<std::vector<std::string>, Positional> positional{};
};

int
process_main_options(int argc, const char* const* argv)
{
  const auto parsed = rfl::cli::read_detailed<MainOptions,
                                              rfl::cli::GnuLastWins,
                                              rfl::cli::AllowAbbreviations,
                                              rfl::cli::ListDelimiter<"">>(
    argc, argv);
  if (!parsed) {
    PRINT(stderr, "{}: {}\n", argv[0], parsed.error().what());
    return EXIT_FAILURE;
  }

  const auto& opts = parsed->value;
  const uint8_t verbosity = *opts.verbose;
  const core::DryRun dry_run = opts.dry_run ? DryRun::yes : DryRun::no;

  // First pass: Handle non-command options that affect command options.
  if (opts.dir) {
    util::setenv("CCACHE_DIR", **opts.dir);
  }
  if (opts.config_path) {
    util::setenv("CCACHE_CONFIGPATH", *opts.config_path);
  }

  StatisticsFormat format = StatisticsFormat::Tab;
  if (opts.format) {
    if (*opts.format == "tab") {
      format = StatisticsFormat::Tab;
    } else if (*opts.format == "json") {
      format = StatisticsFormat::Json;
    } else {
      PRINT(stderr, "Error: unknown format \"{}\"\n", *opts.format);
      return EXIT_FAILURE;
    }
  }

  uint32_t threads = std::max<uint32_t>(2, std::thread::hardware_concurrency());
  if (opts.threads) {
    threads = static_cast<uint32_t>(util::value_or_throw<Error>(
      util::parse_unsigned(**opts.threads,
                           1,
                           std::numeric_limits<uint32_t>::max(),
                           "threads")));
  }

  std::optional<uint64_t> trim_max_size;
  std::optional<util::SizeUnitPrefixType> trim_suffix_type;
  if (opts.trim_max_size) {
    auto [size, suffix_type] =
      util::value_or_throw<Error>(util::parse_size(*opts.trim_max_size));
    trim_max_size = size;
    trim_suffix_type = suffix_type;
  }

  std::optional<std::optional<int8_t>> trim_recompress;
  if (opts.trim_recompress) {
    trim_recompress = parse_compression_level(*opts.trim_recompress);
  }

  const std::optional<fs::path> trim_log = opts.trim_log;
  const std::optional<fs::path> trim_marker = opts.trim_marker;
  const bool trim_lru_mtime = (opts.trim_method == "mtime");

  std::optional<std::string> evict_namespace;
  std::optional<uint64_t> evict_max_age;

  // Second pass: Handle command options in order.
  for (const auto& [opt, arg] : parsed->occurrences) {
    if (opt == "config-path" || opt == "dir" || opt == "dry-run"
        || opt == "format" || opt == "threads" || opt == "trim-log"
        || opt == "trim-marker" || opt == "trim-max-size"
        || opt == "trim-method" || opt == "trim-recompress"
        || opt == "verbose") {
      // Already handled in the first pass.
      continue;
    }

    Config config;
    config.read();
    util::logging::init(config.debug(), config.log_file());

    util::UmaskScope umask_scope(config.umask());

    if (opt == "checksum-file") {
      util::XXH3_128 checksum;
      util::Fd fd(arg == "-" ? STDIN_FILENO : open(arg.c_str(), O_RDONLY));
      if (fd) {
        auto r =
          util::read_fd(*fd, [&checksum](auto data) { checksum.update(data); });
        if (!r) {
          PRINT(stderr, "Error: Failed to checksum {}: {}\n", arg, r.error());
          return EXIT_FAILURE;
        }
        const auto digest = checksum.digest();
        PRINT(stdout, "{}\n", util::format_base16(digest));
      } else {
        PRINT(stderr, "Error: Failed to checksum {}\n", arg);
        return EXIT_FAILURE;
      }
    } else if (opt == "evict-namespace") {
      evict_namespace = arg;
    } else if (opt == "evict-older-than") {
      auto duration = util::value_or_throw<Error>(util::parse_duration(arg));
      evict_max_age =
        std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    } else if (opt == "extract-result") {
      umask_scope.release(); // Use original umask for files outside cache dir
      const auto cache_entry_data = read_from_path_or_stdin(arg);
      if (!cache_entry_data) {
        PRINT(stderr, "Error: \"{}\"", cache_entry_data.error());
        return EXIT_FAILURE;
      }
      std::optional<ResultExtractor::GetRawFilePathFunction> get_raw_file_path;
      if (arg != "-") {
        get_raw_file_path = [&](uint8_t file_number) {
          return storage::local::LocalStorage::get_raw_file_path(arg,
                                                                 file_number);
        };
      }
      ResultExtractor result_extractor(".", get_raw_file_path);
      core::CacheEntry cache_entry(*cache_entry_data);
      const auto payload = cache_entry.payload();

      result::Deserializer result_deserializer(payload);
      result_deserializer.visit(result_extractor);
      cache_entry.verify_checksum();
      return EXIT_SUCCESS;
    } else if (opt == "hash-file") {
      Hash hash;
      const auto result =
        arg == "-" ? hash.hash_fd(STDIN_FILENO) : hash.hash_file(arg);
      if (result) {
        PRINT(stdout, "{}\n", util::format_base16(hash.digest()));
      } else {
        PRINT(stderr, "Error: Failed to hash {}: {}\n", arg, result.error());
        return EXIT_FAILURE;
      }
    } else if (opt == "inspect") {
      return inspect_path(arg);
    } else if (opt == "print-stats") {
      const auto [counters, last_updated] =
        storage::local::LocalStorage(config).get_all_statistics();
      Statistics statistics(counters);
      PRINT(stdout,
            "{}",
            statistics.format_machine_readable(config, last_updated, format));
    } else if (opt == "cleanup") {
      ProgressBar progress_bar("Cleaning...");
      storage::local::LocalStorage(config).clean_all(
        dry_run, [&](double progress) { progress_bar.update(progress); });
    } else if (opt == "clear") {
      ProgressBar progress_bar("Clearing...");
      storage::local::LocalStorage(config).wipe_all(
        [&](double progress) { progress_bar.update(progress); });
      if (isatty(STDOUT_FILENO)) {
        PRINT(stdout, "\n");
      }
    } else if (opt == "help") {
      PRINT(stdout, USAGE_TEXT, fs::path(argv[0]).filename());
      return EXIT_SUCCESS;
    } else if (opt == "get-config") {
      PRINT(stdout, "{}\n", config.get_string_value(arg));
    } else if (opt == "max-files") {
      auto files = util::value_or_throw<Error>(util::parse_unsigned(arg));
      config.set_value_in_file(
        util::pstr(config.config_path()), "max_files", arg);
      if (files == 0) {
        PRINT(stdout, "Unset cache file limit\n");
      } else {
        PRINT(stdout, "Set cache file limit to {}\n", files);
      }
    } else if (opt == "max-size") {
      auto [size, suffix_type] =
        util::value_or_throw<Error>(util::parse_size(arg));
      uint64_t max_size = size;
      config.set_value_in_file(
        util::pstr(config.config_path()), "max_size", arg);
      if (max_size == 0) {
        PRINT(stdout, "Unset cache size limit\n");
      } else {
        PRINT(stdout,
              "Set cache size limit to {}\n",
              util::format_human_readable_size(max_size, suffix_type));
      }
    } else if (opt == "set-config") {
      // Start searching for equal sign at position 1 to improve error message
      // for the -o=K=V case (key "=K" and value "V").
      size_t eq_pos = arg.find('=', 1);
      if (eq_pos == std::string::npos) {
        throw Error(FMT("missing equal sign in \"{}\"", arg));
      }
      std::string key = arg.substr(0, eq_pos);
      std::string value = arg.substr(eq_pos + 1);
      config.set_value_in_file(util::pstr(config.config_path()), key, value);
    } else if (opt == "show-config") {
      config.visit_items(configuration_printer);
    } else if (opt == "show-log-stats") {
      if (config.stats_log().empty()) {
        throw Fatal("No stats log has been configured");
      }
      Statistics statistics(StatsLog(config.stats_log()).read());
      const auto timestamp =
        DirEntry(config.stats_log(), DirEntry::LogOnError::yes).mtime();
      PRINT(
        stdout,
        "{}",
        statistics.format_human_readable(config, timestamp, verbosity, true));
    } else if (opt == "print-log-stats") {
      if (config.stats_log().empty()) {
        throw Fatal("No stats log has been configured");
      }
      Statistics statistics(StatsLog(config.stats_log()).read());
      const auto timestamp =
        DirEntry(config.stats_log(), DirEntry::LogOnError::yes).mtime();
      PRINT(stdout,
            "{}",
            statistics.format_machine_readable(config, timestamp, format));
    } else if (opt == "show-stats") {
      const auto [counters, last_updated] =
        storage::local::LocalStorage(config).get_all_statistics();
      Statistics statistics(counters);
      PRINT(stdout,
            "{}",
            statistics.format_human_readable(
              config, last_updated, verbosity, false));
    } else if (opt == "stop-storage-helpers") {
      storage::Storage storage(config, fs::path(argv[0]).parent_path());
      storage.stop_remote_storage_helpers();
    } else if (opt == "trim-dir") {
      if (!trim_max_size) {
        throw Error("please specify --trim-max-size when using --trim-dir");
      }
      trim_dir({.dry_run = dry_run,
                .dir = arg,
                .marker_path = trim_marker,
                .trim_log_path = trim_log,
                .max_size = *trim_max_size,
                .suffix_type = *trim_suffix_type,
                .lru_mtime = trim_lru_mtime,
                .recompress_level = trim_recompress,
                .threads = threads});
    } else if (opt == "version") {
      PRINT(stdout,
            "{}",
            get_version_text(util::pstr(fs::path(argv[0]).stem()).str()));
    } else if (opt == "print-version") {
      PRINT(stdout, "{}\n", CCACHE_VERSION);
    } else if (opt == "show-compression") {
      ProgressBar progress_bar("Scanning...");
      const auto compression_statistics =
        storage::local::LocalStorage(config).get_compression_statistics(
          threads, [&](double progress) { progress_bar.update(progress); });
      if (isatty(STDOUT_FILENO)) {
        PRINT(stdout, "\n\n");
      }
      print_compression_statistics(config, compression_statistics);
    } else if (opt == "recompress") {
      auto wanted_level = parse_compression_level(arg);

      ProgressBar progress_bar("Recompressing...");
      storage::local::LocalStorage(config).recompress(
        wanted_level, threads, [&](double progress) {
          progress_bar.update(progress);
        });
    } else if (opt == "zero-stats") {
      storage::local::LocalStorage(config).zero_all_statistics();
      PRINT(stdout, "Statistics zeroed\n");
    }
  }

  if (evict_max_age || evict_namespace) {
    Config config;
    config.read();

    ProgressBar progress_bar("Evicting...");
    storage::local::LocalStorage(config).evict(
      dry_run,
      [&](double progress) { progress_bar.update(progress); },
      evict_max_age,
      evict_namespace);
  }

  return EXIT_SUCCESS;
}

} // namespace core
