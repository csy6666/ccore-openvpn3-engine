// SPDX-License-Identifier: MPL-2.0

#include "ccore_openvpn3.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>

// OPENVPN_EXTERNAL_TRANSPORT_FACTORY makes ovpncli.hpp pull in core transport
// headers before ovpncli.cpp installs its default ClientAPI logger.  Select the
// official LogBase path up front so those headers always see valid log macros.
// A process-wide sink is required because the engine runs work on more than one
// thread; the runtime bridge will install a redacting, callback-based sink.
#define OPENVPN_LOG_GLOBAL
#include <openvpn/log/logbase.hpp>
#include <client/ovpncli.cpp>

namespace {

size_t copy_string(const std::string &value, char *output, const size_t capacity)
{
    const size_t required = value.size() + 1;
    if (output == nullptr || capacity == 0)
        return required;
    const size_t count = std::min(value.size(), capacity - 1);
    std::memcpy(output, value.data(), count);
    output[count] = '\0';
    return required;
}

std::string json_escape(const std::string &value)
{
    std::ostringstream out;
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20)
                out << '?';
            else
                out << static_cast<char>(ch);
        }
    }
    return out.str();
}

std::string eval_json(const openvpn::ClientAPI::EvalConfig &result)
{
    std::ostringstream out;
    out << "{\"schema_version\":1"
        << ",\"error\":" << (result.error ? "true" : "false")
        << ",\"message\":\"" << json_escape(result.message) << '"'
        << ",\"autologin\":" << (result.autologin ? "true" : "false")
        << ",\"external_pki\":" << (result.externalPki ? "true" : "false")
        << ",\"remote_protocol\":\"" << json_escape(result.remoteProto) << '"'
        << ",\"dco_compatible\":" << (result.dcoCompatible ? "true" : "false")
        << '}';
    return out.str();
}

std::string error_json(const std::string &message)
{
    return "{\"schema_version\":1,\"error\":true,\"message\":\"" +
           json_escape(message) + "\"}";
}

} // namespace

extern "C" unsigned int ccore_ovpn3_abi_version(void)
{
    return CCORE_OVPN3_ABI_VERSION;
}

extern "C" size_t ccore_ovpn3_version(char *output, const size_t capacity)
{
    return copy_string("openvpn3-core@1512c16622288f3c01da09d3278ac61a86dca26d", output, capacity);
}

extern "C" size_t ccore_ovpn3_license(char *output, const size_t capacity)
{
    return copy_string("MPL-2.0", output, capacity);
}

extern "C" size_t ccore_ovpn3_eval_profile(
    const char *profile,
    char *output,
    const size_t capacity)
{
    if (profile == nullptr)
        return copy_string(error_json("profile is required"), output, capacity);

    try
    {
        openvpn::ClientAPI::Config config;
        config.content = profile;
        config.guiVersion = "ccore-openvpn3 1";
        config.compressionMode = "no";
        config.googleDnsFallback = false;
        config.allowUnusedAddrFamilies = "no";

        openvpn::ClientAPI::OpenVPNClientHelper helper;
        return copy_string(eval_json(helper.eval_config(config)), output, capacity);
    }
    catch (const std::exception &error)
    {
        return copy_string(error_json(error.what()), output, capacity);
    }
    catch (...)
    {
        return copy_string(error_json("unknown OpenVPN 3 evaluation failure"), output, capacity);
    }
}
