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

#include "config.hpp"

#include <ccache/core/atomicfile.hpp>
#include <ccache/core/common.hpp>
#include <ccache/core/exceptions.hpp>
#include <ccache/core/sloppiness.hpp>
#include <ccache/util/assertions.hpp>
#include <ccache/util/configreader.hpp>
#include <ccache/util/direntry.hpp>
#include <ccache/util/environment.hpp>
#include <ccache/util/expected.hpp>
#include <ccache/util/file.hpp>
#include <ccache/util/filesystem.hpp>
#include <ccache/util/format.hpp>
#include <ccache/util/path.hpp>
#include <ccache/util/string.hpp>
#include <ccache/util/tokenizer.hpp>
#include <ccache/util/umaskscope.hpp>
#include <ccache/util/wincompat.hpp>

#ifdef HAVE_PWD_H
#  include <pwd.h>
#endif

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <tl/expected.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef environ
DLLIMPORT extern char** environ;
#endif

// Make room for binary patching at install time. The extra pointer to a buffer
// is needed to prevent the compiler from assuming too much about the string,
// such as its actual length.
const char k_libexecdir_array[4096 + 1] = LIBEXECDIR;
const char* k_libexecdir = k_libexecdir_array;
const char k_sysconfdir_array[4096 + 1] = SYSCONFDIR;
const char* k_sysconfdir = k_sysconfdir_array;

namespace fs = util::filesystem;

using util::DirEntry;
using util::make_path;

namespace config_detail {

const char*
libexec_dir()
{
  return k_libexecdir;
}

} // namespace config_detail

namespace {

bool
is_under_any_safe_dir(const fs::path& dir,
                      const std::vector<std::filesystem::path>& safe_dirs)
{
  // Supported forms:
  //
  //   *                -> matches any directory
  //   /abs/path        -> matches only /abs/path
  //   /abs/path/*      -> matches subdirectories of /abs/path
  return std::ranges::any_of(safe_dirs, [&](const auto& safe_dir) {
    if (safe_dir == "*" || dir == safe_dir) {
      return true;
    }
    if (safe_dir.filename() == "*") {
      const fs::path prefix = safe_dir.parent_path();
      return !prefix.empty() && util::path_starts_with(dir, prefix)
             && dir != prefix;
    }
    return false;
  });
}

bool
is_safe_compiler_check_value(const std::string& value)
{
  return value == "none" || value == "mtime" || value == "content"
         || value.starts_with("string:");
}

std::string
normalize_enum_name(std::string_view name)
{
  if (name == "auto_guess") {
    return "auto";
  }
  std::string result(name);
  std::replace(result.begin(), result.end(), '_', '-');
  return result;
}

template<class Enum>
Enum
parse_config_enum(const std::string& value)
{
  constexpr auto entries = rfl::get_enumerator_array<Enum>();
  for (const auto& [name, enumerator] : entries) {
    if (normalize_enum_name(name) == value) {
      return enumerator;
    }
  }
  // Allow any unknown value for forward compatibility.
  return Enum::auto_guess;
}

template<class Enum>
std::string
format_config_enum(Enum value)
{
  constexpr auto entries = rfl::get_enumerator_array<Enum>();
  for (const auto& [name, enumerator] : entries) {
    if (enumerator == value) {
      return normalize_enum_name(name);
    }
  }
  ASSERT(false);
}

static_assert(rfl::get_enumerator_array<CompilerType>().size() == 11);
static_assert(
  rfl::get_enumerator_array<util::Args::ResponseFileFormat>().size() == 3);
static_assert(rfl::get_enumerator_array<core::Sloppy>().size() == 15);

const std::vector<std::pair<std::string_view, core::Sloppy>>&
sloppiness_entries()
{
  static const auto entries = [] {
    constexpr auto raw = rfl::get_enumerator_array<core::Sloppy>();
    std::vector<std::pair<std::string_view, core::Sloppy>> sorted;
    sorted.reserve(raw.size());
    for (const auto& [name, flag] : raw) {
      if (flag != core::Sloppy::none) {
        sorted.emplace_back(name, flag);
      }
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
      return a.first < b.first;
    });
    return sorted;
  }();
  return entries;
}

bool
parse_bool(const std::string& value,
           const std::optional<std::string>& env_var_key,
           bool negate)
{
  if (env_var_key) {
    // Special rule for boolean settings from the environment: "0", "false",
    // "disable" and "no" (case insensitive) are invalid, and all other values
    // mean true.
    //
    // Previously any value meant true, but this was surprising to users, who
    // might do something like CCACHE_DISABLE=0 and expect ccache to be
    // enabled.
    std::string lower_value = util::to_lowercase(value);
    if (value == "0" || lower_value == "false" || lower_value == "disable"
        || lower_value == "no") {
      throw core::Error(
        FMT("invalid boolean environment variable value \"{}\" (did you mean to"
            " set \"CCACHE_{}{}=true\"?)",
            value,
            negate ? "" : "NO",
            *env_var_key));
    }
    return !negate;
  } else if (value == "true") {
    return true;
  } else if (value == "false") {
    return false;
  } else {
    throw core::Error(FMT("not a boolean value: \"{}\"", value));
  }
}

std::string
format_bool(bool value)
{
  return value ? "true" : "false";
}

core::Sloppiness
parse_sloppiness(const std::string& value)
{
  core::Sloppiness result;
  const auto& entries = sloppiness_entries();

  for (const auto token : util::Tokenizer(value, ", ")) {
    if (token == "no_system_headers") {
      result.insert(core::Sloppy::system_headers);
      continue;
    }
    for (const auto& [name, flag] : entries) {
      if (token == name) {
        result.insert(flag);
        break;
      }
    }
    // else: ignore unknown value for forward compatibility
  }

  return result;
}

std::string
format_sloppiness(core::Sloppiness sloppiness)
{
  std::string result;
  for (const auto& [name, flag] : sloppiness_entries()) {
    if (sloppiness.contains(flag)) {
      if (!result.empty()) {
        result += ", ";
      }
      result += name;
    }
  }
  return result;
}

std::string
format_umask(std::optional<mode_t> umask)
{
  if (umask) {
    return FMT("{:03o}", *umask);
  } else {
    return {};
  }
}

void
verify_absolute_path(const fs::path& path)
{
  if (!path.is_absolute()) {
    throw core::Error(FMT("not an absolute path: \"{}\"", path));
  }
}

bool
parse_line(const std::string& line,
           std::string* key,
           std::string* value,
           std::string* error_message)
{
  std::string_view stripped_line = util::strip_whitespace(line);
  if (stripped_line.empty() || stripped_line[0] == '#') {
    return true;
  }
  size_t equal_pos = stripped_line.find('=');
  if (equal_pos == std::string::npos) {
    *error_message = "missing equal sign";
    return false;
  }
  *key = stripped_line.substr(0, equal_pos);
  *value = stripped_line.substr(equal_pos + 1);
  *key = util::strip_whitespace(*key);
  *value = util::strip_whitespace(*value);
  return true;
}

std::unordered_map<std::string, std::string>
create_cmdline_settings_map(const std::vector<std::string>& settings)
{
  std::unordered_map<std::string, std::string> result;
  for (const auto& setting : settings) {
    DEBUG_ASSERT(setting.find('=') != std::string::npos);
    std::string key;
    std::string value;
    std::string error_message;
    bool ok = parse_line(setting, &key, &value, &error_message);
    ASSERT(ok);
    if (!key.empty()) {
      result.insert_or_assign(std::move(key), std::move(value));
    }
  }
  return result;
}

#ifndef _WIN32

fs::path
default_cache_dir(const fs::path& home_dir)
{
#  ifdef __APPLE__
  return home_dir / "Library/Caches/ccache";
#  else
  return home_dir / ".cache/ccache";
#  endif
}

fs::path
default_config_dir(const fs::path& home_dir)
{
#  ifdef __APPLE__
  return home_dir / "Library/Preferences/ccache";
#  else
  return home_dir / ".config/ccache";
#  endif
}

#endif // !_WIN32

fs::path
home_directory()
{
#ifdef _WIN32
  auto user_profile = util::getenv_path("USERPROFILE");
  if (user_profile) {
    return *user_profile;
  }
  throw core::Fatal(
    "The USERPROFILE environment variable must be set to your user profile"
    " folder");
#else
  auto home = util::getenv_path("HOME");
  if (home) {
    return *home;
  }
#  ifdef HAVE_GETPWUID
  {
    struct passwd* pwd = getpwuid(getuid());
    if (pwd) {
      return pwd->pw_dir;
    }
  }
#  endif
  throw core::Fatal(
    "Could not determine home directory from $HOME or getpwuid(3)");
#endif
}

std::string
response_file_format_to_string(
  util::Args::ResponseFileFormat response_file_format)
{
  return format_config_enum(response_file_format);
}

template<class T>
struct UnwrapRename
{
  using Type = T;
};

template<rfl::internal::StringLiteral Name, class T>
struct UnwrapRename<rfl::Rename<Name, T>>
{
  using Type = T;
};

template<class T>
using UnwrapRenameT = typename UnwrapRename<std::remove_cvref_t<T>>::Type;

template<class T>
decltype(auto)
unwrap_option(T& opt) noexcept
{
  if constexpr (rfl::internal::is_rename_v<std::remove_cvref_t<T>>) {
    return opt.value();
  } else {
    return opt;
  }
}

template<class View, size_t... Is>
consteval bool
are_view_fields_sorted(std::index_sequence<Is...>)
{
  constexpr std::array<std::string_view, sizeof...(Is)> names = {
    rfl::tuple_element_t<Is, typename View::Fields>::name()...};
  for (size_t i = 1; i < names.size(); ++i) {
    if (!(names[i - 1] < names[i])) {
      return false;
    }
  }
  return true;
}

using ConfigView = rfl::view_t<ConfigData>;
static_assert(std::is_aggregate_v<ConfigData>);
static_assert(rfl::num_fields<ConfigData> == 48);
static_assert(
  are_view_fields_sorted<ConfigView>(
    std::make_index_sequence<rfl::num_fields<ConfigData>>{}),
  "ConfigData fields must be declared in strict alphabetical order of "
  "canonical keys");

} // namespace

namespace rfl {

template<>
struct Reflector<CompilerType>
{
  using ReflType = rfl::Literal<"auto",
                                "clang",
                                "clang-cl",
                                "gcc",
                                "icl",
                                "icx",
                                "icx-cl",
                                "msvc",
                                "nvcc",
                                "qcc",
                                "other">;

  static ReflType
  from(const CompilerType& v)
  {
    return ReflType(compiler_type_to_string(v));
  }

  static CompilerType
  to(const ReflType& v)
  {
    return parse_config_enum<CompilerType>(v.str());
  }
};

template<>
struct Reflector<util::Args::ResponseFileFormat>
{
  using ReflType = rfl::Literal<"auto", "posix", "windows">;

  static ReflType
  from(const util::Args::ResponseFileFormat& v)
  {
    return ReflType(response_file_format_to_string(v));
  }

  static util::Args::ResponseFileFormat
  to(const ReflType& v)
  {
    return parse_config_enum<util::Args::ResponseFileFormat>(v.str());
  }
};

template<>
struct Reflector<core::Sloppiness>
{
  using ReflType = std::string;

  static ReflType
  from(const core::Sloppiness& v)
  {
    return format_sloppiness(v);
  }

  static core::Sloppiness
  to(const ReflType& v)
  {
    return parse_sloppiness(v);
  }
};

} // namespace rfl

namespace config_detail {

using ConfigConstView = decltype(rfl::to_view(std::declval<const ConfigData&>()));
using ConfigMutView = decltype(rfl::to_view(std::declval<ConfigData&>()));

struct ConfigMeta
{
  struct KeyInfo
  {
    std::string_view canonical_key;
    DirConfigPolicy dir_config_policy;
    std::string (*format_fn)(const Config&, const ConfigConstView&);
    void (*set_fn)(Config&,
                   ConfigMutView&,
                   const std::string& value,
                   const std::optional<std::string>& env_var_key,
                   bool negate);
  };

  std::unordered_map<std::string_view, KeyInfo> key_info;
  std::unordered_map<std::string_view, std::string_view> env_to_key;

  template<class T>
  static std::string
  format_field(const Config& cfg, std::string_view key, const T& val)
  {
    if constexpr (std::is_same_v<T, bool>) {
      return format_bool(val);
    } else if constexpr (std::is_same_v<T, std::string>) {
      return val;
    } else if constexpr (std::is_same_v<T, std::filesystem::path>) {
      return val.string();
    } else if constexpr (std::is_same_v<T, std::vector<std::filesystem::path>>) {
      return util::join_path_list(val);
    } else if constexpr (std::is_same_v<T, CompilerType>) {
      return compiler_type_to_string(val);
    } else if constexpr (std::is_same_v<T, util::Args::ResponseFileFormat>) {
      return response_file_format_to_string(val);
    } else if constexpr (std::is_same_v<T, core::Sloppiness>) {
      return format_sloppiness(val);
    } else if constexpr (std::is_same_v<T, std::optional<mode_t>>) {
      return format_umask(val);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      if (key == "max_size") {
        auto result =
          util::format_human_readable_size(val, cfg.m_size_prefix_type);
        if (result.ends_with(" bytes")) {
          // Special case to make the output parsable by util::parse_size.
          result.resize(result.size() - 6);
        }
        return result;
      }
      return FMT("{}", val);
    } else {
      return FMT("{}", val);
    }
  }

  template<class T>
  static void
  set_field(Config& cfg,
            std::string_view key,
            T& field,
            const std::string& value,
            const std::optional<std::string>& env_var_key,
            bool negate)
  {
    if constexpr (std::is_same_v<T, bool>) {
      field = parse_bool(value, env_var_key, negate);
    } else if constexpr (std::is_same_v<T, std::string>) {
      field = value;
    } else if constexpr (std::is_same_v<T, std::filesystem::path>) {
      if (key == "cache_dir") {
        cfg.set_cache_dir(value);
      } else if (key == "temporary_dir") {
        field = value;
        cfg.m_temporary_dir_configured_explicitly = true;
      } else {
        field = value;
      }
    } else if constexpr (std::is_same_v<T, std::vector<std::filesystem::path>>) {
      if (key == "base_dir") {
        cfg.set_base_dirs(util::split_path_list(value));
      } else if (key == "ceiling_dirs") {
        cfg.set_ceiling_dirs(util::split_path_list(value));
      } else if (key == "safe_dirs") {
        cfg.set_safe_dirs(util::split_path_list(value));
      } else {
        field = util::split_path_list(value);
      }
    } else if constexpr (std::is_same_v<T, int8_t>) {
      field = static_cast<int8_t>(util::value_or_throw<core::Error>(
        util::parse_signed(value, INT8_MIN, INT8_MAX, key)));
    } else if constexpr (std::is_same_v<T, uint8_t>) {
      const std::string_view label =
        (key == "debug_level") ? "debug level" : key;
      field = static_cast<uint8_t>(util::value_or_throw<core::Error>(
        util::parse_unsigned(value, 0, UINT8_MAX, label)));
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      if (key == "max_size") {
        const auto [size, prefix_type] =
          util::value_or_throw<core::Error>(util::parse_size(value));
        field = size;
        cfg.m_size_prefix_type = prefix_type;
      } else {
        field = util::value_or_throw<core::Error>(
          util::parse_unsigned(value, std::nullopt, std::nullopt, key));
      }
    } else if constexpr (std::is_same_v<T, CompilerType>) {
      field = parse_config_enum<CompilerType>(value);
    } else if constexpr (std::is_same_v<T, util::Args::ResponseFileFormat>) {
      field = parse_config_enum<util::Args::ResponseFileFormat>(value);
    } else if constexpr (std::is_same_v<T, core::Sloppiness>) {
      field = parse_sloppiness(value);
    } else if constexpr (std::is_same_v<T, std::optional<mode_t>>) {
      if (!value.empty()) {
        field = util::value_or_throw<core::Error>(util::parse_umask(value));
      }
    }
  }

  template<size_t I>
  static void
  register_field(ConfigMeta& meta)
  {
    using Field = rfl::tuple_element_t<I, typename ConfigView::Fields>;
    using RawFieldT = std::remove_pointer_t<typename Field::Type>;
    using OptionT = UnwrapRenameT<RawFieldT>;
    constexpr std::string_view key = Field::name();

    auto* const format_fn =
      +[](const Config& cfg, const ConfigConstView& view) -> std::string {
      const auto& opt = unwrap_option(*rfl::get<static_cast<int>(I)>(view));
      return format_field(cfg, Field::name(), opt.get());
    };
    auto* const set_fn = +[](Config& cfg,
                             ConfigMutView& view,
                             const std::string& val,
                             const std::optional<std::string>& env_var_key,
                             bool negate) {
      auto& opt = unwrap_option(*rfl::get<static_cast<int>(I)>(view));
      set_field(cfg, Field::name(), opt.get(), val, env_var_key, negate);
    };

    const KeyInfo info{key, OptionT::dir_policy, format_fn, set_fn};
    meta.key_info.emplace(key, info);
    if constexpr (!OptionT::key_alias.empty()) {
      meta.key_info.emplace(OptionT::key_alias, info);
    }
    if constexpr (!OptionT::env_var.empty()) {
      meta.env_to_key.emplace(OptionT::env_var, key);
    }
    if constexpr (!OptionT::env_alias.empty()) {
      meta.env_to_key.emplace(OptionT::env_alias, key);
    }
  }

  template<size_t... Is>
  static ConfigMeta
  build(std::index_sequence<Is...>)
  {
    ConfigMeta meta;
    meta.key_info.reserve(sizeof...(Is) + 4);
    meta.env_to_key.reserve(sizeof...(Is) + 4);
    (register_field<Is>(meta), ...);
    return meta;
  }

  template<size_t... Is>
  static void
  check_consistency(std::index_sequence<Is...>)
  {
    std::unordered_set<std::string_view> seen_keys;
    std::unordered_set<std::string_view> seen_env_vars;

    auto check_one = [&]<size_t I>() {
      using Field = rfl::tuple_element_t<I, typename ConfigView::Fields>;
      using RawFieldT = std::remove_pointer_t<typename Field::Type>;
      using OptionT = UnwrapRenameT<RawFieldT>;
      constexpr std::string_view key = Field::name();

      if (!seen_keys.insert(key).second) {
        throw core::Error(FMT("duplicate config key \"{}\"", key));
      }
      if constexpr (!OptionT::key_alias.empty()) {
        if (!seen_keys.insert(OptionT::key_alias).second) {
          throw core::Error(
            FMT("duplicate config key alias \"{}\"", OptionT::key_alias));
        }
      }
      if constexpr (OptionT::env_var.empty()) {
        throw core::Error(FMT("missing env var for config key \"{}\"", key));
      } else {
        if (!seen_env_vars.insert(OptionT::env_var).second) {
          throw core::Error(
            FMT("duplicate env var \"{}\"", OptionT::env_var));
        }
      }
      if constexpr (!OptionT::env_alias.empty()) {
        if (!seen_env_vars.insert(OptionT::env_alias).second) {
          throw core::Error(
            FMT("duplicate env var alias \"{}\"", OptionT::env_alias));
        }
      }
    };

    (check_one.template operator()<Is>(), ...);
  }
};

const ConfigMeta&
reflected_config_meta()
{
  static const ConfigMeta meta = ConfigMeta::build(
    std::make_index_sequence<rfl::num_fields<ConfigData>>{});
  return meta;
}

} // namespace config_detail

void
Config::set_ceiling_dirs(const std::vector<std::filesystem::path>& value)
{
  auto& dirs = m_data.ceiling_dirs.get();
  dirs.clear();
  for (const auto& path : value) {
    verify_absolute_path(path);
    dirs.push_back(util::lexically_normal(path));
  }
}

void
Config::set_safe_dirs(const std::vector<std::filesystem::path>& value)
{
  auto& dirs = m_data.safe_dirs.get();
  dirs.clear();
  for (const auto& path : value) {
    if (path == "*") {
      dirs.push_back(path);
    } else {
      verify_absolute_path(path);
      dirs.push_back(util::lexically_normal(path));
    }
  }
}

std::string
compiler_type_to_string(CompilerType compiler_type)
{
  return format_config_enum(compiler_type);
}

void
Config::read(const std::vector<std::string>& cmdline_config_settings)
{
  auto cmdline_settings_map =
    create_cmdline_settings_map(cmdline_config_settings);

  const fs::path home_dir = home_directory();

  set_ceiling_dirs({home_dir});

  const util::DirEntry legacy_ccache_dir = home_dir / ".ccache";
#ifdef _WIN32
  auto env_appdata = util::getenv_path("APPDATA");
  auto env_local_appdata = util::getenv_path("LOCALAPPDATA");
#else
  auto env_xdg_cache_home = util::getenv_path("XDG_CACHE_HOME");
  auto env_xdg_config_home = util::getenv_path("XDG_CONFIG_HOME");
#endif

  auto env_ccache_configpath = util::getenv_path("CCACHE_CONFIGPATH");
  if (env_ccache_configpath) {
    set_config_path(*env_ccache_configpath);
  } else {
    // Only used for ccache tests:
    auto env_ccache_configpath2 = util::getenv_path("CCACHE_CONFIGPATH2");

    fs::path sysconfdir(k_sysconfdir);
#ifdef _WIN32
    auto program_data = util::getenv_path("ALLUSERSPROFILE");
    if (program_data) {
      sysconfdir = *program_data / "ccache";
    }
#endif

    set_system_config_path(env_ccache_configpath2 ? *env_ccache_configpath2
                                                  : sysconfdir / "ccache.conf");
    // Config priority 5: system-wide config.
    //
    // Note: A missing config file in SYSCONFDIR is OK so don't check return
    // value.
    update_from_file(system_config_path());

    auto env_ccache_dir = util::getenv_path("CCACHE_DIR");
    auto cmdline_cache_dir = cmdline_settings_map.find("cache_dir");

    fs::path config_dir;
    if (cmdline_cache_dir != cmdline_settings_map.end()) {
      config_dir = cmdline_cache_dir->second;
    } else if (env_ccache_dir && !env_ccache_dir->empty()) {
      config_dir = *env_ccache_dir;
    } else if (!cache_dir().empty() && !env_ccache_dir) {
      config_dir = cache_dir();
    } else if (legacy_ccache_dir.is_directory()) {
      config_dir = legacy_ccache_dir.path();
#ifdef _WIN32
    } else if (env_local_appdata
               && fs::exists(*env_local_appdata / "ccache/ccache.conf")) {
      config_dir = *env_local_appdata / "ccache";
    } else if (env_appdata && fs::exists(*env_appdata / "ccache/ccache.conf")) {
      config_dir = make_path(*env_appdata, "ccache");
    } else if (env_local_appdata) {
      config_dir = *env_local_appdata / "ccache";
    } else {
      throw core::Fatal(
        "could not find configuration file and the LOCALAPPDATA environment"
        " variable is not set");
    }
#else
    } else if (env_xdg_config_home) {
      config_dir = *env_xdg_config_home / "ccache";
    } else {
      config_dir = default_config_dir(home_dir);
    }
#endif
    set_config_path(config_dir / "ccache.conf");
  }

  const fs::path& cache_dir_before_config_file_was_read = cache_dir();

  // Config priority 4: cache-specific config.
  update_from_file(config_path());
  // Ignore cache_dir set in cache-specific configuration file:
  set_cache_dir(cache_dir_before_config_file_was_read);

  // Config priority 3: directory-specific config.
  //
  // We need special handling of CCACHE_CEILING_DIRS/CCACHE_CEILING_MARKERS and
  // ceiling_dirs/ceiling_markers from the command line since we want those
  // settings to be applied before other settings from the directory config, but
  // other settings in the directory config should have lower priority than
  // those from environment and command line.
  if (!env_ccache_configpath) {
    auto ceiling_dirs_entry = cmdline_settings_map.find("ceiling_dirs");
    if (ceiling_dirs_entry != cmdline_settings_map.end()) {
      set_ceiling_dirs(util::split_path_list(ceiling_dirs_entry->second));
    } else if (getenv("CCACHE_CEILING_DIRS")) {
      set_ceiling_dirs(util::getenv_path_list("CCACHE_CEILING_DIRS"));
    }
    auto ceiling_markers_entry = cmdline_settings_map.find("ceiling_markers");
    if (ceiling_markers_entry != cmdline_settings_map.end()) {
      set_ceiling_markers(util::split_path_list(ceiling_markers_entry->second));
    } else if (getenv("CCACHE_CEILING_MARKERS")) {
      set_ceiling_markers(util::getenv_path_list("CCACHE_CEILING_MARKERS"));
    }

    auto safe_dirs_entry = cmdline_settings_map.find("safe_dirs");
    if (safe_dirs_entry != cmdline_settings_map.end()) {
      set_safe_dirs(util::split_path_list(safe_dirs_entry->second));
    } else if (getenv("CCACHE_SAFE_DIRS")) {
      set_safe_dirs(util::getenv_path_list("CCACHE_SAFE_DIRS"));
    }

    auto dir_config = find_directory_config();
    if (dir_config) {
      update_from_dir_config_file(*dir_config);
      m_dir_config_path = std::move(*dir_config);
    }
  }

  // Config priority 2: environment variables.
  update_from_environment();

  // Config priority 1: command-line settings.
  update_from_map(cmdline_settings_map);

  if (cache_dir().empty()) {
    if (legacy_ccache_dir.is_directory()) {
      set_cache_dir(legacy_ccache_dir.path());
#ifdef _WIN32
    } else if (env_local_appdata) {
      set_cache_dir(*env_local_appdata / "ccache");
    } else {
      throw core::Fatal(
        "could not find cache directory and the LOCALAPPDATA environment"
        " variable is not set");
    }
#else
    } else if (env_xdg_cache_home) {
      set_cache_dir(*env_xdg_cache_home / "ccache");
    } else {
      set_cache_dir(default_cache_dir(home_dir));
    }
#endif
  }
  // else: cache_dir was set explicitly via environment or via system config.
}

const char*
Config::libexec_dir()
{
  return config_detail::libexec_dir();
}

const char*
Config::sysconf_dir()
{
  return k_sysconfdir;
}

std::string
Config::json_schema()
{
  return rfl::json::to_schema<ConfigData>(rfl::json::pretty);
}

std::string
Config::to_json() const
{
  return rfl::json::write(m_data, rfl::json::pretty);
}

const fs::path&
Config::config_path() const
{
  return m_config_path;
}

const fs::path&
Config::dir_config_path() const
{
  return m_dir_config_path;
}

const fs::path&
Config::system_config_path() const
{
  return m_system_config_path;
}

void
Config::set_base_dirs(const std::vector<std::filesystem::path>& value)
{
  auto& dirs = m_data.base_dir.get();
  dirs.clear();
  for (const auto& path : value) {
    verify_absolute_path(path);
    dirs.push_back(util::lexically_normal(path));
  }
}

void
Config::set_config_path(const fs::path& path)
{
  m_config_path = util::lexically_normal(path);
}

void
Config::set_system_config_path(const fs::path& path)
{
  m_system_config_path = util::lexically_normal(path);
}

bool
Config::update_from_file(const fs::path& path)
{
  auto config_content = util::read_file<std::string>(path);
  if (!config_content) {
    return false;
  }

  util::ConfigReader reader(*config_content);

  while (true) {
    auto item_result = reader.read_next_item();
    if (!item_result) {
      throw core::Error(FMT("{}:{}: {}",
                            path,
                            item_result.error().line_number,
                            item_result.error().message));
    }

    auto& item = *item_result;
    if (!item) {
      break; // EOF
    }

    try {
      set_item(item->key, item->value, std::nullopt, false, util::pstr(path));
    } catch (const core::Error& e) {
      throw core::Error(FMT("{}:{}: {}", path, item->line_number, e.what()));
    }
  }

  return true;
}

bool
Config::update_from_dir_config_file(const fs::path& path)
{
  auto config_content = util::read_file<std::string>(path);
  if (!config_content) {
    return false;
  }

  util::ConfigReader reader(*config_content);

  const fs::path dir = path.parent_path();
  const bool allow_unsafe = is_under_any_safe_dir(dir, m_data.safe_dirs.get());
  const auto& meta = config_detail::reflected_config_meta();

  while (true) {
    auto item_result = reader.read_next_item();
    if (!item_result) {
      throw core::Error(FMT("{}:{}: {}",
                            path,
                            item_result.error().line_number,
                            item_result.error().message));
    }

    auto& item = *item_result;
    if (!item) {
      break; // EOF
    }

    const auto it = meta.key_info.find(item->key);
    if (it == meta.key_info.end()) {
      throw core::Error(FMT("{}:{}: unknown configuration option \"{}\"",
                            path,
                            item->line_number,
                            item->key));
    }

    const auto policy = it->second.dir_config_policy;
    if (policy == DirConfigPolicy::reject) {
      throw core::Error(
        FMT("{}:{}: configuration option \"{}\" is not allowed in directory"
            " configuration files",
            path,
            item->line_number,
            item->key));
    }

    if (policy == DirConfigPolicy::unsafe && !allow_unsafe) {
      if (it->second.canonical_key == "compiler_check") {
        // Value-dependent exception: allow compiler_check as long as it doesn't
        // invoke a command.
        auto expanded_value = util::expand_environment_variables(item->value);
        if (!expanded_value) {
          throw core::Error(
            FMT("{}:{}: {}", path, item->line_number, expanded_value.error()));
        }
        if (!is_safe_compiler_check_value(*expanded_value)) {
          throw core::Error(FMT(
            "{}:{}: configuration option \"{}\" is considered unsafe when it"
            " invokes a command and is only allowed in directory configuration"
            " files located under safe_dirs",
            path,
            item->line_number,
            item->key));
        }
      } else {
        throw core::Error(FMT(
          "{}:{}: configuration option \"{}\" is considered unsafe and is only"
          " allowed in directory configuration files located under safe_dirs",
          path,
          item->line_number,
          item->key));
      }
    }

    try {
      set_item(item->key, item->value, std::nullopt, false, util::pstr(path));
    } catch (const core::Error& e) {
      throw core::Error(FMT("{}:{}: {}", path, item->line_number, e.what()));
    }
  }

  return true;
}

void
Config::update_from_map(const std::unordered_map<std::string, std::string>& map)
{
  for (const auto& [key, value] : map) {
    try {
      set_item(key, value, std::nullopt, false, "command line");
    } catch (core::Error& e) {
      throw core::Error(
        FMT("when parsing command line config \"{}\": {}", key, e.what()));
    }
  }
}

void
Config::update_from_environment()
{
  const auto& meta = config_detail::reflected_config_meta();

  for (char** env = environ; *env; ++env) {
    std::string setting = *env;
    const std::string prefix = "CCACHE_";
    if (!setting.starts_with(prefix)) {
      continue;
    }
    size_t equal_pos = setting.find('=');
    if (equal_pos == std::string::npos) {
      continue;
    }

    std::string key = setting.substr(prefix.size(), equal_pos - prefix.size());
    std::string value = setting.substr(equal_pos + 1);
    bool negate = key.starts_with("NO");
    if (negate) {
      key = key.substr(2);
    }

    auto it = meta.env_to_key.find(key);
    if (it == meta.env_to_key.end()) {
      // Ignore unknown keys.
      continue;
    }
    const auto& config_key = it->second;

    try {
      set_item(config_key, value, key, negate, "environment");
    } catch (const core::Error& e) {
      throw core::Error(
        FMT("CCACHE_{}{}: {}", negate ? "NO" : "", key, e.what()));
    }
  }
}

std::string
Config::get_string_value(const std::string& key) const
{
  const auto& meta = config_detail::reflected_config_meta();
  const auto it = meta.key_info.find(key);
  if (it == meta.key_info.end()) {
    throw core::Error(FMT("unknown configuration option \"{}\"", key));
  }
  const auto view = rfl::to_view(m_data);
  return it->second.format_fn(*this, view);
}

void
Config::set_value_in_file(const std::string& path,
                          const std::string& key,
                          const std::string& value) const
{
  util::UmaskScope umask_scope(m_data.umask.get());

  const auto& meta = config_detail::reflected_config_meta();
  if (meta.key_info.find(key) == meta.key_info.end()) {
    throw core::Error(FMT("unknown configuration option \"{}\"", key));
  }

  // Verify that the value is valid; set_item will throw if not.
  Config dummy_config;
  dummy_config.set_item(key, value, std::nullopt, false, "");

  const fs::path resolved_path = fs::canonical(path).value_or(path);
  if (!fs::is_regular_file(resolved_path)) {
    core::ensure_dir_exists(resolved_path.parent_path());
    util::throw_on_error<core::Error>(
      util::write_file(resolved_path, ""),
      FMT("failed to write to {}: ", resolved_path));
  }

  auto config_content = util::read_file<std::string>(path);
  if (!config_content) {
    throw core::Error(FMT("failed to read {}: {}", path, strerror(errno)));
  }

  std::string result;
  result.reserve(config_content->size() + key.size() + 3 + value.size() + 1);
  bool key_found = false;

  util::ConfigReader reader(*config_content);
  while (true) {
    auto item_result = reader.read_next_raw_item();
    if (!item_result) {
      throw core::Error(FMT("failed to read {}: {}:{}",
                            path,
                            item_result.error().line_number,
                            item_result.error().message));
    }

    auto& item = *item_result;
    if (!item) {
      break; // EOF
    }

    if (item->key == key) {
      // Copy everything before this entry
      result.append(*config_content, 0, item->value_start_pos);
      result.append(value);
      result.append(*config_content,
                    item->value_start_pos + item->value_length);
      key_found = true;
      break;
    }
  }

  if (!key_found) {
    result = std::move(*config_content);
    if (!result.empty() && result.back() != '\n') {
      result += '\n';
    }
    result += FMT("{} = {}\n", key, value);
  }

  core::AtomicFile output(resolved_path, core::AtomicFile::Mode::text);
  output.write(result);
  output.commit();
}

void
Config::visit_items(const ItemVisitor& item_visitor) const
{
  rfl::to_view(m_data).apply([&](const auto& field) {
    using Field = std::remove_cvref_t<decltype(field)>;
    const std::string key(Field::name());
    const auto it = m_origins.find(key);
    const std::string origin = it != m_origins.end() ? it->second : "default";
    const auto& opt = unwrap_option(*field.value());
    item_visitor(
      key,
      config_detail::ConfigMeta::format_field(*this, key, opt.get()),
      origin);
  });
}

void
Config::set_item(const std::string_view& key,
                 const std::string& unexpanded_value,
                 const std::optional<std::string>& env_var_key,
                 bool negate,
                 const std::string& origin)
{
  const auto& meta = config_detail::reflected_config_meta();
  const auto it = meta.key_info.find(key);
  if (it == meta.key_info.end()) {
    // Ignore unknown keys.
    return;
  }

  std::string value = util::value_or_throw<core::Error>(
    util::expand_environment_variables(unexpanded_value));

  auto view = rfl::to_view(m_data);
  it->second.set_fn(*this, view, value, env_var_key, negate);

  const std::string_view canonical_key = it->second.canonical_key;
  const auto& [element, inserted] = m_origins.emplace(canonical_key, origin);
  if (!inserted) {
    element->second = origin;
  }
}

void
Config::check_key_tables_consistency()
{
  config_detail::ConfigMeta::check_consistency(
    std::make_index_sequence<rfl::num_fields<ConfigData>>{});
}

fs::path
Config::default_temporary_dir() const
{
  const auto dir = get_xdg_runtime_tmp_dir();
  return !dir.empty() ? dir : m_data.cache_dir.get() / "tmp";
}

std::filesystem::path
Config::get_xdg_runtime_tmp_dir()
{
#ifdef _WIN32
  return {};
#else
  const char* const xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (xdg_runtime_dir && DirEntry(xdg_runtime_dir).is_directory()) {
    fs::path dir = FMT("{}/ccache-tmp", xdg_runtime_dir);
    if (fs::create_directories(dir) && access(dir.c_str(), W_OK) == 0) {
      return dir;
    }
  }
  return {};
#endif
}

std::optional<std::filesystem::path>
Config::find_directory_config() const
{
  auto cwd = fs::current_path();
  if (!cwd) {
    return std::nullopt;
  }

  fs::path dir = util::lexically_normal(*cwd);

#ifndef _WIN32
  const uid_t euid = geteuid();
#endif

  DirEntry dir_entry(dir);

  while (true) {
    // Stop if dir is a ceiling dir (exclusive: don't check this dir).
    for (const auto& ceiling : m_data.ceiling_dirs.get()) {
      if (dir == ceiling) {
        return std::nullopt;
      }
    }

    if (!dir_entry.is_directory()) {
      return std::nullopt;
    }

#ifndef _WIN32
    // Stop if dir is owned by another user (sanity/performance check).
    if (dir_entry.uid() != euid) {
      return std::nullopt;
    }
#endif

    // Check for a directory-specific config file.
    const fs::path candidate = dir / "ccache.conf";
    const DirEntry candidate_entry(candidate);
    if (candidate_entry.is_regular_file()) {
#ifndef _WIN32
      if (candidate_entry.uid() != euid) {
        throw core::Error(
          FMT("directory-specific configuration file {} owned by another user"
              " (owner {} != uid {})",
              candidate,
              candidate_entry.uid(),
              euid));
      }
      if (candidate_entry.mode() & S_IWOTH) {
        throw core::Error(
          FMT("directory-specific configuration file {} is world-writable",
              candidate));
      }
#endif
      return candidate;
    }

    // Stop if this dir contains a ceiling marker.
    for (const auto& marker : m_data.ceiling_markers.get()) {
      if (DirEntry(dir / marker)) {
        return std::nullopt;
      }
    }

    // Stop at filesystem root or filesystem boundary.
    fs::path parent = dir.parent_path();
    if (parent == dir) {
      return std::nullopt;
    }
    DirEntry parent_entry(parent);
    if (!parent_entry.is_directory()
        || parent_entry.device() != dir_entry.device()) {
      return std::nullopt;
    }

    dir = std::move(parent);
    dir_entry = std::move(parent_entry);
  }
}
