// Copyright (C) 2019-2026 Joel Rosdahl and other contributors
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

#pragma once

#include <ccache/core/sloppiness.hpp>
#include <ccache/util/args.hpp>
#include <ccache/util/noncopyable.hpp>
#include <ccache/util/path.hpp>
#include <ccache/util/string.hpp>

#include <rfl/DefaultVal.hpp>
#include <rfl/Description.hpp>
#include <rfl/Rename.hpp>
#include <rfl/internal/StringLiteral.hpp>

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

enum class CompilerType {
  auto_guess,
  clang,
  clang_cl,
  gcc,
  icl,
  icx,
  icx_cl,
  msvc,
  nvcc,
  qcc,
  other
};

std::string compiler_type_to_string(CompilerType compiler_type);

enum class DirConfigPolicy : uint8_t {
  allow,  // always allow
  reject, // always reject with an error
  unsafe, // allow if config is in directory matching safe_dirs, error otherwise
};

namespace config_detail {

const char* libexec_dir();

#ifndef _WIN32
inline constexpr bool k_default_inode_cache = true;
#else
// Support is experimental on Windows so usage is off by default.
inline constexpr bool k_default_inode_cache = false;
#endif

struct ConfigMeta;

} // namespace config_detail

template<class T,
         rfl::internal::StringLiteral Desc,
         rfl::internal::StringLiteral EnvVar,
         DirConfigPolicy Policy = DirConfigPolicy::allow,
         rfl::internal::StringLiteral EnvAlias = "",
         rfl::internal::StringLiteral KeyAlias = "">
struct ConfigOption
{
  using ValueType = T;
  using Type = rfl::Description<Desc, rfl::DefaultVal<T>>;
  using ReflectionType = Type;
  static constexpr bool is_transparent_wrapper_ = true;

  static constexpr std::string_view description = Desc.string_view();
  static constexpr std::string_view env_var = EnvVar.string_view();
  static constexpr DirConfigPolicy dir_policy = Policy;
  static constexpr std::string_view env_alias = EnvAlias.string_view();
  static constexpr std::string_view key_alias = KeyAlias.string_view();

  ConfigOption() = default;
  ConfigOption(const T& v) : value_(rfl::DefaultVal<T>(v)) {}
  ConfigOption(T&& v) : value_(rfl::DefaultVal<T>(std::move(v))) {}
  ConfigOption(const Type& v) : value_(v) {}
  ConfigOption(Type&& v) : value_(std::move(v)) {}

  template<class U>
    requires(!std::is_same_v<std::remove_cvref_t<U>, ConfigOption>
             && !std::is_same_v<std::remove_cvref_t<U>, Type>
             && !std::is_same_v<std::remove_cvref_t<U>, T>
             && std::is_convertible_v<U, T>)
  ConfigOption(U&& v) : value_(rfl::DefaultVal<T>(std::forward<U>(v)))
  {
  }

  ConfigOption(const ConfigOption&) = default;
  ConfigOption(ConfigOption&&) = default;
  ConfigOption& operator=(const ConfigOption&) = default;
  ConfigOption& operator=(ConfigOption&&) = default;

  ConfigOption&
  operator=(const T& v)
  {
    value_.value().value() = v;
    return *this;
  }

  ConfigOption&
  operator=(T&& v)
  {
    value_.value().value() = std::move(v);
    return *this;
  }

  const Type&
  value() const noexcept
  {
    return value_;
  }

  Type&
  value() noexcept
  {
    return value_;
  }

  const Type&
  reflection() const noexcept
  {
    return value_;
  }

  const T&
  get() const noexcept
  {
    return value_.value().value();
  }

  T&
  get() noexcept
  {
    return value_.value().value();
  }

  const T&
  operator*() const noexcept
  {
    return get();
  }

  T&
  operator*() noexcept
  {
    return get();
  }

  const T*
  operator->() const noexcept
  {
    return &get();
  }

  T*
  operator->() noexcept
  {
    return &get();
  }

  Type value_;
};

// Single Source of Truth for all 48 ccache configuration options.
// Fields MUST remain in strict alphabetical order of canonical keys (enforced
// by static_assert in config.cpp).
struct ConfigData
{
  ConfigOption<bool,
               "Rewrite relative paths in stderr to absolute paths",
               "ABSSTDERR">
    absolute_paths_in_stderr = false;

  ConfigOption<std::vector<std::filesystem::path>,
               "Base directories for rewriting absolute paths to relative",
               "BASEDIR">
    base_dir;

  ConfigOption<std::filesystem::path,
               "Directory where ccache keeps cached compiler outputs",
               "DIR",
               DirConfigPolicy::reject>
    cache_dir;

  ConfigOption<std::vector<std::filesystem::path>,
               "Directories at which to stop searching for directory configs",
               "CEILING_DIRS",
               DirConfigPolicy::reject>
    ceiling_dirs;

  ConfigOption<std::vector<std::filesystem::path>,
               "Marker files/dirs that stop searching for directory configs",
               "CEILING_MARKERS",
               DirConfigPolicy::reject>
    ceiling_markers = std::vector<std::filesystem::path>{".git"};

  ConfigOption<std::string,
               "Override the compiler to use",
               "COMPILER",
               DirConfigPolicy::unsafe,
               "CC">
    compiler;

  ConfigOption<std::string,
               "How to verify that the compiler has not changed",
               "COMPILERCHECK",
               DirConfigPolicy::unsafe>
    compiler_check = "mtime";

  ConfigOption<CompilerType,
               "Explicit compiler type instead of auto-detection",
               "COMPILERTYPE">
    compiler_type = CompilerType::auto_guess;

  ConfigOption<bool, "Compress files stored in the cache", "COMPRESS">
    compression = true;

  ConfigOption<int8_t,
               "Compression level (0 uses default level)",
               "COMPRESSLEVEL">
    compression_level = 0;

  ConfigOption<bool, "Enable debug mode", "DEBUG"> debug = false;

  ConfigOption<std::filesystem::path,
               "Directory in which to write debug files",
               "DEBUGDIR",
               DirConfigPolicy::unsafe>
    debug_dir;

  ConfigOption<uint8_t, "Amount of debug information written", "DEBUGLEVEL">
    debug_level = 2;

  ConfigOption<bool, "Enable depend mode", "DEPEND"> depend_mode = false;

  ConfigOption<bool, "Enable direct mode", "DIRECT"> direct_mode = true;

  ConfigOption<bool, "Disable ccache and invoke compiler directly", "DISABLE">
    disable = false;

  ConfigOption<std::string,
               "Extra files to include in the hash",
               "EXTRAFILES">
    extra_files_to_hash;

  ConfigOption<bool,
               "Clone files instead of copying when supported by filesystem",
               "FILECLONE">
    file_clone = false;

  ConfigOption<bool, "Hard-link object files instead of copying", "HARDLINK">
    hard_link = false;

  ConfigOption<bool,
               "Include current working directory in the hash",
               "HASHDIR">
    hash_dir = true;

  ConfigOption<std::string,
               "Paths to headers to exclude from the manifest",
               "IGNOREHEADERS">
    ignore_headers_in_manifest;

  ConfigOption<std::string,
               "Compiler options to ignore when computing the hash",
               "IGNOREOPTIONS">
    ignore_options;

  ConfigOption<bool,
               "Cache file content hashes by inode to speed up repeated lookups",
               "INODECACHE">
    inode_cache = config_detail::k_default_inode_cache;

  ConfigOption<bool,
               "Keep comments when running the preprocessor",
               "COMMENTS">
    keep_comments_cpp = false;

  ConfigOption<std::vector<std::filesystem::path>,
               "Directories searched for storage helper programs",
               "LIBEXEC_DIRS",
               DirConfigPolicy::unsafe>
    libexec_dirs =
      std::vector<std::filesystem::path>{config_detail::libexec_dir()};

  ConfigOption<std::filesystem::path,
               "Path to log file for ccache debug logs",
               "LOGFILE",
               DirConfigPolicy::unsafe>
    log_file;

  ConfigOption<uint64_t,
               "Maximum number of files allowed in the cache (0 = unlimited)",
               "MAXFILES",
               DirConfigPolicy::reject>
    max_files = 0;

  ConfigOption<uint64_t,
               "Maximum size of the cache in bytes (0 = unlimited)",
               "MAXSIZE",
               DirConfigPolicy::reject>
    max_size = 5ULL * 1024 * 1024 * 1024;

  ConfigOption<std::string,
               "Prefix used by MSVC /showIncludes output",
               "MSVC_DEP_PREFIX">
    msvc_dep_prefix = "Note: including file:";

  ConfigOption<bool, "Force UTF-8 output for MSVC", "MSVC_UTF8">
    msvc_utf8 = true;

  rfl::Rename<"namespace",
              ConfigOption<std::string,
                           "Namespace string added to the hash",
                           "NAMESPACE">>
    namespace_;

  ConfigOption<std::string,
               "Search PATH used to locate the real compiler",
               "PATH",
               DirConfigPolicy::unsafe>
    path;

  ConfigOption<bool,
               "Use external checksum file for precompiled headers",
               "PCH_EXTSUM">
    pch_external_checksum = false;

  ConfigOption<std::string,
               "Command prefix prepended when invoking the compiler",
               "PREFIX",
               DirConfigPolicy::unsafe>
    prefix_command;

  ConfigOption<std::string,
               "Command prefix prepended when invoking the preprocessor",
               "PREFIX_CPP",
               DirConfigPolicy::unsafe>
    prefix_command_cpp;

  ConfigOption<bool, "Do not write new entries to the cache", "READONLY">
    read_only = false;

  ConfigOption<bool,
               "Only read from direct mode cache without updating it",
               "READONLY_DIRECT">
    read_only_direct = false;

  ConfigOption<bool,
               "Ignore existing cache hits and overwrite with fresh output",
               "RECACHE">
    recache = false;

  ConfigOption<bool, "Use only remote storage without local cache", "REMOTE_ONLY">
    remote_only = false;

  ConfigOption<std::string,
               "Remote storage backend URL(s)",
               "REMOTE_STORAGE",
               DirConfigPolicy::unsafe,
               "SECONDARY_STORAGE",
               "secondary_storage">
    remote_storage;

  ConfigOption<bool,
               "Write local cache hits back to remote storage",
               "RESHARE">
    reshare = false;

  ConfigOption<util::Args::ResponseFileFormat,
               "Quoting format used for @response files",
               "RESPONSE_FILE_FORMAT">
    response_file_format = util::Args::ResponseFileFormat::auto_guess;

  ConfigOption<std::vector<std::filesystem::path>,
               "Directories where unsafe directory config options are allowed",
               "SAFE_DIRS",
               DirConfigPolicy::reject>
    safe_dirs;

  ConfigOption<core::Sloppiness,
               "Comma-separated list of relaxed cache-correctness checks",
               "SLOPPINESS">
    sloppiness;

  ConfigOption<bool, "Update cache statistics counters", "STATS"> stats = true;

  ConfigOption<std::filesystem::path,
               "Path to statistics log file",
               "STATSLOG",
               DirConfigPolicy::unsafe>
    stats_log;

  ConfigOption<std::filesystem::path,
               "Directory for temporary files",
               "TEMPDIR",
               DirConfigPolicy::unsafe>
    temporary_dir;

  ConfigOption<std::optional<mode_t>,
               "File creation umask for files created by ccache",
               "UMASK",
               DirConfigPolicy::unsafe>
    umask;
};

class Config : util::NonCopyable
{
public:
  Config() = default;

  void read(const std::vector<std::string>& cmdline_config_settings = {});

  static const char* libexec_dir();
  static const char* sysconf_dir();

  static std::string json_schema();
  std::string to_json() const;

  bool absolute_paths_in_stderr() const;
  const std::vector<std::filesystem::path>& base_dirs() const;
  const std::filesystem::path& cache_dir() const;
  const std::vector<std::filesystem::path>& ceiling_dirs() const;
  const std::vector<std::filesystem::path>& ceiling_markers() const;
  const std::string& compiler() const;
  const std::string& compiler_check() const;
  CompilerType compiler_type() const;
  bool compression() const;
  int8_t compression_level() const;
  bool debug() const;
  const std::filesystem::path& debug_dir() const;
  uint8_t debug_level() const;
  bool depend_mode() const;
  bool direct_mode() const;
  bool disable() const;
  const std::string& extra_files_to_hash() const;
  bool file_clone() const;
  bool hard_link() const;
  bool hash_dir() const;
  const std::string& ignore_headers_in_manifest() const;
  const std::string& ignore_options() const;
  bool inode_cache() const;
  bool keep_comments_cpp() const;
  const std::vector<std::filesystem::path>& libexec_dirs() const;
  const std::filesystem::path& log_file() const;
  uint64_t max_files() const;
  uint64_t max_size() const;
  const std::string& msvc_dep_prefix() const;
  bool msvc_utf8() const;
  const std::string& path() const;
  bool pch_external_checksum() const;
  const std::string& prefix_command() const;
  const std::string& prefix_command_cpp() const;
  bool read_only() const;
  bool read_only_direct() const;
  bool recache() const;
  bool remote_only() const;
  const std::string& remote_storage() const;
  bool reshare() const;
  util::Args::ResponseFileFormat response_file_format() const;
  const std::vector<std::filesystem::path>& safe_dirs() const;
  core::Sloppiness sloppiness() const;
  bool stats() const;
  const std::filesystem::path& stats_log() const;
  const std::string& namespace_() const;
  const std::filesystem::path& temporary_dir() const;
  std::optional<mode_t> umask() const;

  // Return true for Clang, clang-cl and icx (not on Windows).
  bool is_compiler_group_clang() const;

  // Return true for GCC and QCC (QNX compiler, which is GCC-based).
  bool is_compiler_group_gcc() const;

  // Return true for MSVC (cl.exe), clang-cl, icl, icx-cl, and icx (on Windows).
  bool is_compiler_group_msvc() const;

  util::SizeUnitPrefixType size_unit_prefix_type() const;
  std::filesystem::path default_temporary_dir() const;
  static std::filesystem::path get_xdg_runtime_tmp_dir();

  void set_base_dir(const std::filesystem::path& value);
  void set_base_dirs(const std::vector<std::filesystem::path>& value);
  void set_cache_dir(const std::filesystem::path& value);
  void set_ceiling_dirs(const std::vector<std::filesystem::path>& value);
  void set_ceiling_markers(const std::vector<std::filesystem::path>& value);
  void set_compiler(const std::string& value);
  void set_compiler_type(CompilerType value);
  void set_debug(bool value);
  void set_depend_mode(bool value);
  void set_direct_mode(bool value);
  void set_file_clone(bool value);
  void set_hard_link(bool value);
  void set_ignore_options(const std::string& value);
  void set_inode_cache(bool value);
  void set_max_files(uint64_t value);
  void set_msvc_dep_prefix(const std::string& value);
  void set_safe_dirs(const std::vector<std::filesystem::path>& value);
  void set_msvc_utf8(bool value);
  void set_temporary_dir(const std::filesystem::path& value);

  // Where to write configuration changes.
  const std::filesystem::path& config_path() const;
  // Directory-specific config (if any).
  const std::filesystem::path& dir_config_path() const;
  // System (read-only) configuration file (if any).
  const std::filesystem::path& system_config_path() const;

  void set_config_path(const std::filesystem::path& path);
  void set_system_config_path(const std::filesystem::path& path);

  using ItemVisitor = std::function<void(const std::string& key,
                                         const std::string& value,
                                         const std::string& origin)>;

  // Set config values from a configuration file.
  //
  // Returns false if the file can't be opened, otherwise true. Throws Error on
  // invalid configuration values.
  bool update_from_file(const std::filesystem::path& path);

  // Set config values from a map with key-value pairs.
  //
  // Throws Error on invalid configuration values.
  void update_from_map(const std::unordered_map<std::string, std::string>& map);

  // Set config values from environment variables.
  //
  // Throws Error on invalid configuration values.
  void update_from_environment();

  // Get a config value in string form given a key.
  std::string get_string_value(const std::string& key) const;

  void visit_items(const ItemVisitor& item_visitor) const;

  void set_value_in_file(const std::string& path,
                         const std::string& key,
                         const std::string& value) const;

  // Called from unit tests.
  static void check_key_tables_consistency();

private:
  friend struct config_detail::ConfigMeta;

  std::filesystem::path m_config_path;
  std::filesystem::path m_dir_config_path;
  std::filesystem::path m_system_config_path;

  ConfigData m_data;

  bool m_temporary_dir_configured_explicitly = false;
  util::SizeUnitPrefixType m_size_prefix_type =
    util::SizeUnitPrefixType::binary;

  std::unordered_map<std::string /*key*/, std::string /*origin*/> m_origins;

  void set_item(const std::string_view& key,
                const std::string& unexpanded_value,
                const std::optional<std::string>& env_var_key,
                bool negate,
                const std::string& origin);

  std::optional<std::filesystem::path> find_directory_config() const;
  bool update_from_dir_config_file(const std::filesystem::path& path);
};

inline bool
Config::absolute_paths_in_stderr() const
{
  return m_data.absolute_paths_in_stderr.get();
}

inline const std::vector<std::filesystem::path>&
Config::base_dirs() const
{
  return m_data.base_dir.get();
}

inline const std::filesystem::path&
Config::cache_dir() const
{
  return m_data.cache_dir.get();
}

inline const std::vector<std::filesystem::path>&
Config::ceiling_dirs() const
{
  return m_data.ceiling_dirs.get();
}

inline const std::vector<std::filesystem::path>&
Config::ceiling_markers() const
{
  return m_data.ceiling_markers.get();
}

inline const std::string&
Config::compiler() const
{
  return m_data.compiler.get();
}

inline const std::string&
Config::compiler_check() const
{
  return m_data.compiler_check.get();
}

inline CompilerType
Config::compiler_type() const
{
  return m_data.compiler_type.get();
}

inline bool
Config::is_compiler_group_clang() const
{
  const auto ct = compiler_type();
  return ct == CompilerType::clang
#ifndef _WIN32
         || ct == CompilerType::icx
#endif
         || ct == CompilerType::clang_cl;
}

inline bool
Config::is_compiler_group_gcc() const
{
  const auto ct = compiler_type();
  return ct == CompilerType::gcc || ct == CompilerType::qcc;
}

inline bool
Config::is_compiler_group_msvc() const
{
  const auto ct = compiler_type();
  return ct == CompilerType::msvc || ct == CompilerType::clang_cl
         || ct == CompilerType::icl
#ifdef _WIN32
         || ct == CompilerType::icx
#endif
         || ct == CompilerType::icx_cl;
}

inline bool
Config::compression() const
{
  return m_data.compression.get();
}

inline int8_t
Config::compression_level() const
{
  return m_data.compression_level.get();
}

inline bool
Config::debug() const
{
  return m_data.debug.get();
}

inline const std::filesystem::path&
Config::debug_dir() const
{
  return m_data.debug_dir.get();
}

inline uint8_t
Config::debug_level() const
{
  return m_data.debug_level.get();
}

inline bool
Config::depend_mode() const
{
  return m_data.depend_mode.get();
}

inline bool
Config::direct_mode() const
{
  return m_data.direct_mode.get();
}

inline bool
Config::disable() const
{
  return m_data.disable.get();
}

inline const std::string&
Config::extra_files_to_hash() const
{
  return m_data.extra_files_to_hash.get();
}

inline bool
Config::file_clone() const
{
  return m_data.file_clone.get();
}

inline bool
Config::hard_link() const
{
  return m_data.hard_link.get();
}

inline bool
Config::hash_dir() const
{
  return m_data.hash_dir.get();
}

inline const std::string&
Config::ignore_headers_in_manifest() const
{
  return m_data.ignore_headers_in_manifest.get();
}

inline const std::string&
Config::ignore_options() const
{
  return m_data.ignore_options.get();
}

inline bool
Config::inode_cache() const
{
  return m_data.inode_cache.get();
}

inline bool
Config::keep_comments_cpp() const
{
  return m_data.keep_comments_cpp.get();
}

inline const std::vector<std::filesystem::path>&
Config::libexec_dirs() const
{
  return m_data.libexec_dirs.get();
}

inline const std::filesystem::path&
Config::log_file() const
{
  return m_data.log_file.get();
}

inline uint64_t
Config::max_files() const
{
  return m_data.max_files.get();
}

inline uint64_t
Config::max_size() const
{
  return m_data.max_size.get();
}

inline const std::string&
Config::msvc_dep_prefix() const
{
  return m_data.msvc_dep_prefix.get();
}

inline bool
Config::msvc_utf8() const
{
  return m_data.msvc_utf8.get();
}

inline const std::string&
Config::path() const
{
  return m_data.path.get();
}

inline bool
Config::pch_external_checksum() const
{
  return m_data.pch_external_checksum.get();
}

inline const std::string&
Config::prefix_command() const
{
  return m_data.prefix_command.get();
}

inline const std::string&
Config::prefix_command_cpp() const
{
  return m_data.prefix_command_cpp.get();
}

inline bool
Config::read_only() const
{
  return m_data.read_only.get();
}

inline bool
Config::read_only_direct() const
{
  return m_data.read_only_direct.get();
}

inline bool
Config::recache() const
{
  return m_data.recache.get();
}

inline bool
Config::remote_only() const
{
  return m_data.remote_only.get();
}

inline const std::string&
Config::remote_storage() const
{
  return m_data.remote_storage.get();
}

inline bool
Config::reshare() const
{
  return m_data.reshare.get();
}

inline util::Args::ResponseFileFormat
Config::response_file_format() const
{
  const auto fmt = m_data.response_file_format.get();
  if (fmt != util::Args::ResponseFileFormat::auto_guess) {
    return fmt;
  }

  return is_compiler_group_msvc() ? util::Args::ResponseFileFormat::windows
                                  : util::Args::ResponseFileFormat::posix;
}

inline const std::vector<std::filesystem::path>&
Config::safe_dirs() const
{
  return m_data.safe_dirs.get();
}

inline core::Sloppiness
Config::sloppiness() const
{
  return m_data.sloppiness.get();
}

inline bool
Config::stats() const
{
  return m_data.stats.get();
}

inline const std::filesystem::path&
Config::stats_log() const
{
  return m_data.stats_log.get();
}

inline const std::string&
Config::namespace_() const
{
  return m_data.namespace_.value().get();
}

inline const std::filesystem::path&
Config::temporary_dir() const
{
  return m_data.temporary_dir.get();
}

inline std::optional<mode_t>
Config::umask() const
{
  return m_data.umask.get();
}

inline util::SizeUnitPrefixType
Config::size_unit_prefix_type() const
{
  return m_size_prefix_type;
}

inline void
Config::set_base_dir(const std::filesystem::path& value)
{
  set_base_dirs({value});
}

inline void
Config::set_cache_dir(const std::filesystem::path& value)
{
  m_data.cache_dir = util::lexically_normal(value);
  if (!m_temporary_dir_configured_explicitly) {
    m_data.temporary_dir = default_temporary_dir();
  }
}

inline void
Config::set_ceiling_markers(const std::vector<std::filesystem::path>& value)
{
  m_data.ceiling_markers = value;
}

inline void
Config::set_compiler(const std::string& value)
{
  m_data.compiler = value;
}

inline void
Config::set_compiler_type(CompilerType value)
{
  m_data.compiler_type = value;
}

inline void
Config::set_depend_mode(bool value)
{
  m_data.depend_mode = value;
}

inline void
Config::set_debug(bool value)
{
  m_data.debug = value;
}

inline void
Config::set_direct_mode(bool value)
{
  m_data.direct_mode = value;
}

inline void
Config::set_file_clone(const bool value)
{
  m_data.file_clone = value;
}

inline void
Config::set_hard_link(const bool value)
{
  m_data.hard_link = value;
}

inline void
Config::set_ignore_options(const std::string& value)
{
  m_data.ignore_options = value;
}

inline void
Config::set_inode_cache(bool value)
{
  m_data.inode_cache = value;
}

inline void
Config::set_max_files(uint64_t value)
{
  m_data.max_files = value;
}

inline void
Config::set_msvc_dep_prefix(const std::string& value)
{
  m_data.msvc_dep_prefix = value;
}

inline void
Config::set_msvc_utf8(bool value)
{
  m_data.msvc_utf8 = value;
}

inline void
Config::set_temporary_dir(const std::filesystem::path& value)
{
  m_data.temporary_dir = util::lexically_normal(value);
}
