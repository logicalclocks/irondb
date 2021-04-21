/*
  Copyright (c) 2018, 2021, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "mock_server_plugin.h"

#include <array>
#include <climits>  // PATH_MAX
#include <stdexcept>
#include <string>
#include <system_error>  // error_code

#include "mysql.h"  // mysql_ssl_mode
#include "mysql/harness/config_option.h"
#include "mysql/harness/config_parser.h"
#include "mysql/harness/logging/logging.h"
#include "mysql/harness/plugin.h"
#include "mysql/harness/stdx/filesystem.h"
#include "mysql/harness/string_utils.h"  // split_string
#include "mysql/harness/tls_context.h"
#include "mysql/harness/tls_server_context.h"
#include "mysql_server_mock.h"
#include "mysqlrouter/plugin_config.h"

IMPORT_LOG_FUNCTIONS()

static constexpr std::array<std::pair<const char *, mysql_ssl_mode>, 3>
    allowed_ssl_modes = {{
        {"DISABLED", SSL_MODE_DISABLED},
        {"PREFERRED", SSL_MODE_PREFERRED},
        {"REQUIRED", SSL_MODE_REQUIRED},
    }};

static constexpr const char kSectionName[]{"mock_server"};

static mysql_ssl_mode get_option_ssl_mode(
    const mysql_harness::ConfigSection *section,
    const mysql_harness::ConfigOption &option) {
  auto res = option.get_option_string(section);
  if (!res) {
    throw std::invalid_argument(res.error().message());
  }

  // convert name to upper-case to get case-insenstive comparison.
  auto name = res.value();
  std::transform(name.begin(), name.end(), name.begin(), ::toupper);

  // check if the mode is known
  const auto it =
      std::find_if(allowed_ssl_modes.begin(), allowed_ssl_modes.end(),
                   [name](auto const &allowed_ssl_mode) {
                     return name == allowed_ssl_mode.first;
                   });
  if (it != allowed_ssl_modes.end()) {
    return it->second;
  }

  // build list of allowed modes, but don't mention the default case.
  std::string allowed_names;
  for (const auto &allowed_ssl_mode : allowed_ssl_modes) {
    if (!allowed_names.empty()) {
      allowed_names.append(",");
    }

    allowed_names += allowed_ssl_mode.first;
  }

  throw std::invalid_argument("invalid value '" + res.value() + "' for " +
                              option.name() +
                              ". Allowed are: " + allowed_names + ".");
}

class PluginConfig : public mysqlrouter::BasePluginConfig {
 public:
  std::string trace_filename;
  std::vector<std::string> module_prefixes;
  std::string srv_address;
  uint16_t srv_port;
  std::string srv_protocol;
  std::string ssl_ca;
  std::string ssl_capath;
  std::string ssl_cert;
  std::string ssl_key;
  std::string ssl_cipher;
  std::string ssl_crl;
  std::string ssl_crlpath;
  mysql_ssl_mode ssl_mode;
  std::string tls_version;

  std::vector<std::string> get_option_strings(
      const mysql_harness::ConfigSection *section,
      const std::string &option_name) {
    auto val = get_option_string(section, option_name);

    return mysql_harness::split_string(val, ',');
  }

  explicit PluginConfig(const mysql_harness::ConfigSection *section)
      : mysqlrouter::BasePluginConfig(section),
        trace_filename(get_option_string(section, "filename")),
        module_prefixes(get_option_strings(section, "module_prefix")),
        srv_address(get_option_string(section, "bind_address")),
        srv_port(get_uint_option<uint16_t>(section, "port")),
        srv_protocol(get_option_string(section, "protocol")),
        ssl_ca(get_option_string(section, "ssl_ca")),
        ssl_capath(get_option_string(section, "ssl_capath")),
        ssl_cert(get_option_string(section, "ssl_cert")),
        ssl_key(get_option_string(section, "ssl_key")),
        ssl_cipher(get_option_string(section, "ssl_cipher")),
        ssl_crl(get_option_string(section, "ssl_crl")),
        ssl_crlpath(get_option_string(section, "ssl_crlpath")),
        ssl_mode(get_option_ssl_mode(
            section, mysql_harness::ConfigOption("ssl_mode", "disabled"))),
        tls_version(get_option_string(section, "tls_version")) {}

  std::string get_default(const std::string &option) const override {
    std::error_code ec;
    const auto cwd = stdx::filesystem::current_path(ec);
    if (ec) {
      throw std::system_error(ec);
    }

    const std::map<std::string, std::string> defaults{
        {"bind_address", "0.0.0.0"},
        {"module_prefix", cwd.native()},
        {"port", "3306"},
        {"protocol", "classic"},
        {"ssl_mode", "DISABLED"},
    };

    auto it = defaults.find(option);
    if (it == defaults.end()) {
      return std::string();
    }
    return it->second;
  }

  bool is_required(const std::string &option) const override {
    if (option == "filename") return true;
    return false;
  }
};

static std::map<std::string, std::shared_ptr<server_mock::MySQLServerMock>>
    mock_servers;

static void init(mysql_harness::PluginFuncEnv *env) {
  const mysql_harness::AppInfo *info = get_app_info(env);

  try {
    if (info->config != nullptr) {
      for (const mysql_harness::ConfigSection *section :
           info->config->sections()) {
        if (section->name != kSectionName) {
          continue;
        }

        PluginConfig config{section};
        const std::string key = section->name + ":" + section->key;

        TlsServerContext tls_server_ctx;

        if (config.ssl_mode != SSL_MODE_DISABLED) {
          if (!config.tls_version.empty()) {
            const std::map<std::string, TlsVersion> known_tls_versions{
                {"TLSv1", TlsVersion::TLS_1_0},
                {"TLSv1.1", TlsVersion::TLS_1_1},
                {"TLSv1.2", TlsVersion::TLS_1_2},
                {"TLSv1.3", TlsVersion::TLS_1_3},
            };

            auto const it = known_tls_versions.find(config.tls_version);
            if (it == known_tls_versions.end()) {
              throw std::runtime_error(
                  "setting 'tls_version=" + config.tls_version +
                  "' failed. Unknown TLS version.");
            }

            TlsVersion min_version = it->second;
            TlsVersion max_version = min_version;
            tls_server_ctx.version_range(min_version, max_version);
          }

          if (!config.ssl_ca.empty() || !config.ssl_capath.empty()) {
            auto res = tls_server_ctx.ssl_ca(config.ssl_ca, config.ssl_capath);
            if (!res) {
              throw std::system_error(
                  res.error(), "setting ssl_ca='" + config.ssl_ca +
                                   "' or ssl_capath='" + config.ssl_capath +
                                   "' failed");
            }
          }

          if (config.ssl_key.empty() || config.ssl_cert.empty()) {
            throw std::invalid_argument(
                "if ssl_mode is not DISABLED, ssl_key and "
                "ssl_cert MUST be set. ssl_key is " +
                (config.ssl_key.empty() ? "empty"
                                        : "'" + config.ssl_key + "'") +
                ", ssl_cert is " +
                (config.ssl_cert.empty() ? "empty"
                                         : "'" + config.ssl_cert + "'"));
          } else {
            auto res = tls_server_ctx.load_key_and_cert(config.ssl_key,
                                                        config.ssl_cert);
            if (!res) {
              throw std::system_error(res.error(),
                                      "setting ssl_key='" + config.ssl_key +
                                          "' or ssl_cert='" + config.ssl_cert +
                                          "' failed");
            }
          }

          if (!config.ssl_cipher.empty()) {
            auto res = tls_server_ctx.cipher_list(config.ssl_cipher);
            if (!res) {
              throw std::system_error(
                  res.error(),
                  "setting ssl_cipher='" + config.ssl_cipher + "' failed");
            }
          }

          if (!config.ssl_crl.empty() || !config.ssl_crlpath.empty()) {
            auto res = tls_server_ctx.crl(config.ssl_crl, config.ssl_crlpath);
            if (!res) {
              throw std::system_error(
                  res.error(), "setting ssl_crl='" + config.ssl_crl +
                                   "' or ssl_crlpath='" + config.ssl_crlpath +
                                   "' failed");
            }
          }

          // if the client presents a cert, verify it.
          tls_server_ctx.verify(TlsVerify::PEER);
        }

        mock_servers.emplace(std::make_pair(
            key, std::make_shared<server_mock::MySQLServerMock>(
                     config.trace_filename, config.module_prefixes,
                     config.srv_address, config.srv_port, config.srv_protocol,
                     0, std::move(tls_server_ctx), config.ssl_mode)));

        MockServerComponent::get_instance().register_server(
            mock_servers.at(key));
      }
    }
  } catch (const std::invalid_argument &exc) {
    set_error(env, mysql_harness::kConfigInvalidArgument, "%s", exc.what());
  } catch (const std::exception &exc) {
    set_error(env, mysql_harness::kRuntimeError, "%s", exc.what());
  } catch (...) {
    set_error(env, mysql_harness::kUndefinedError, "Unexpected exception");
  }
}

static void start(mysql_harness::PluginFuncEnv *env) {
  const mysql_harness::ConfigSection *section = get_config_section(env);

  std::string name;
  if (!section->key.empty()) {
    name = section->name + ":" + section->key;
  } else {
    name = section->name;
  }

  try {
    auto srv = mock_servers.at(name);

    srv->run(env);
  } catch (const std::invalid_argument &exc) {
    set_error(env, mysql_harness::kConfigInvalidArgument, "%s", exc.what());
  } catch (const std::runtime_error &exc) {
    set_error(env, mysql_harness::kRuntimeError, "%s: %s", name.c_str(),
              exc.what());
  } catch (const std::exception &exc) {
    set_error(env, mysql_harness::kUndefinedError, "%s: %s", name.c_str(),
              exc.what());
  } catch (...) {
    set_error(env, mysql_harness::kUndefinedError, "Unexpected exception");
  }
}

static const std::array<const char *, 3> required = {{
    "logger",
    "router_openssl",
    "router_protobuf",
}};

extern "C" {
mysql_harness::Plugin MOCK_SERVER_EXPORT harness_plugin_mock_server = {
    mysql_harness::PLUGIN_ABI_VERSION,       // abi-version
    mysql_harness::ARCHITECTURE_DESCRIPTOR,  // arch
    "Routing MySQL connections between MySQL clients/connectors and "
    "servers",  // name
    VERSION_NUMBER(0, 0, 1),
    // requires
    required.size(), required.data(),
    // conflicts
    0, nullptr,
    init,     // init
    nullptr,  // deinit
    start,    // start
    nullptr,  // stop
    true,     // declares_readiness
};
}
