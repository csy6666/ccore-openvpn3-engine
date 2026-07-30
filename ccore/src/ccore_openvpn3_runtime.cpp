// SPDX-License-Identifier: MPL-2.0

#include "ccore_openvpn3.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Keep logging configuration identical to ccore_openvpn3.cpp.  The core log
// sink intentionally remains unset: runtime diagnostics cross the ABI only as
// bounded structured events, never as profile or credential-bearing log text.
#define OPENVPN_LOG_GLOBAL
#include <openvpn/log/logbase.hpp>
#include <client/ovpncli.hpp>
#include <openvpn/transport/client/extern/config.hpp>
#include <openvpn/transport/client/tcpcli.hpp>
#include <openvpn/transport/client/udpcli.hpp>

namespace {

constexpr size_t kMaxErrorText = 1024;
constexpr size_t kMaxTunnelItems = 512;
constexpr size_t kMaxPacketSize = 65535;

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

std::string bounded_text(std::string value)
{
    for (char &ch : value)
    {
        if (ch == '\r' || ch == '\n' || ch == '\0')
            ch = ' ';
    }
    if (value.size() > kMaxErrorText)
        value.resize(kMaxErrorText);
    return value;
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

struct TunnelState
{
    int mtu = 0;
    std::vector<std::string> local_addresses;
    std::vector<std::string> routes;
    std::vector<std::string> dns_servers;
};

void append_unique(std::vector<std::string> &items, const std::string &value)
{
    if (items.size() >= kMaxTunnelItems)
        throw std::runtime_error("tunnel configuration item limit exceeded");
    if (std::find(items.begin(), items.end(), value) == items.end())
        items.push_back(value);
}

void write_json_array(std::ostringstream &out, const std::vector<std::string> &items)
{
    out << '[';
    for (size_t index = 0; index < items.size(); ++index)
    {
        if (index != 0)
            out << ',';
        out << '"' << json_escape(items[index]) << '"';
    }
    out << ']';
}

std::string tunnel_json(const TunnelState &state)
{
    std::ostringstream out;
    out << "{\"schema_version\":1,\"reason\":\"connected\",\"mtu\":" << state.mtu;
    out << ",\"local_addresses\":";
    write_json_array(out, state.local_addresses);
    out << ",\"routes\":";
    write_json_array(out, state.routes);
    out << ",\"dns_servers\":";
    write_json_array(out, state.dns_servers);
    out << ",\"selected_cipher\":\"\",\"selected_auth\":\"\"}";
    return out.str();
}

class BridgeLease
{
  public:
    BridgeLease(const ccore_ovpn3_client_options &options, const uint64_t bridge_id)
        : user_data_(options.user_data), bridge_id_(bridge_id), close_(options.close_bridge)
    {
    }

    ~BridgeLease()
    {
        close();
    }

    void close()
    {
        bool expected = false;
        if (closed_.compare_exchange_strong(expected, true) && close_ != nullptr)
            close_(user_data_, bridge_id_);
    }

  private:
    uint64_t user_data_;
    uint64_t bridge_id_;
    ccore_ovpn3_close_bridge_fn close_;
    std::atomic<bool> closed_{false};
};

class BridgeTransportFactory final : public openvpn::TransportClientFactory
{
  public:
    BridgeTransportFactory(openvpn::TransportClientFactory::Ptr delegate,
                           std::shared_ptr<BridgeLease> lease)
        : delegate_(std::move(delegate)), lease_(std::move(lease))
    {
    }

    openvpn::TransportClient::Ptr new_transport_client_obj(
        openvpn_io::io_context &io_context,
        openvpn::TransportClientParent *parent) override
    {
        return delegate_->new_transport_client_obj(io_context, parent);
    }

    bool is_relay() override
    {
        return delegate_->is_relay();
    }

    void process_push(const openvpn::OptionList &options) override
    {
        delegate_->process_push(options);
    }

  private:
    openvpn::TransportClientFactory::Ptr delegate_;
    std::shared_ptr<BridgeLease> lease_;
};

class RuntimeClient final : public openvpn::ClientAPI::OpenVPNClient
{
  public:
    explicit RuntimeClient(const ccore_ovpn3_client_options &options)
        : options_(options), profile_(options.profile == nullptr ? "" : options.profile),
          username_(options.username == nullptr ? "" : options.username),
          password_(options.password == nullptr ? "" : options.password)
    {
    }

    ~RuntimeClient() override
    {
        stop_and_join();
    }

    int configure()
    {
        try
        {
            openvpn::ClientAPI::Config config;
            config.content = profile_;
            config.guiVersion = "ccore-openvpn3 2";
            config.compressionMode = "no";
            config.googleDnsFallback = false;
            config.allowUnusedAddrFamilies = "no";
            config.tunPersist = true;
            config.enableRouteEmulation = false;
            config.enableLegacyAlgorithms = options_.enable_legacy_algorithms != 0;
            config.enableNonPreferredDCAlgorithms = options_.enable_legacy_algorithms != 0;

            const openvpn::ClientAPI::EvalConfig eval = eval_config(config);
            if (eval.error)
                return fail("profile evaluation failed: " + eval.message);
            if (eval.externalPki)
                return fail("external PKI profiles are not supported by ABI v2");

            if (!eval.autologin || !username_.empty() || !password_.empty())
            {
                openvpn::ClientAPI::ProvideCreds credentials;
                credentials.username = username_;
                credentials.password = password_;
                const openvpn::ClientAPI::Status status = provide_creds(credentials);
                if (status.error)
                    return fail("credential setup failed: " + status.message);
            }
            configured_.store(true);
            return 0;
        }
        catch (const std::exception &error)
        {
            return fail(std::string("configure OpenVPN 3: ") + error.what());
        }
        catch (...)
        {
            return fail("configure OpenVPN 3: unknown failure");
        }
    }

    int start_async()
    {
        if (!configured_.load())
            return fail("client is not configured");
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true))
            return fail("client has already been started");
        finished_.store(false);
        stopping_.store(false);
        try
        {
            worker_ = std::thread([this]()
                                  { run(); });
            return 0;
        }
        catch (const std::exception &error)
        {
            started_.store(false);
            finished_.store(true);
            return fail(std::string("start OpenVPN 3 thread: ") + error.what());
        }
    }

    int reconnect_now()
    {
        if (!started_.load() || finished_.load() || stopping_.load())
            return fail("client is not running");
        reconnect(0);
        return 0;
    }

    int stop_and_join()
    {
        stopping_.store(true);
        ready_.store(false);
        if (started_.load() && !finished_.load())
            stop();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
            worker_.join();
        close_packet_fd();
        close_bridges();
        finished_.store(true);
        packet_cv_.notify_all();
        return 0;
    }

    bool ready() const
    {
        return ready_.load() && !stopping_.load() && !finished_.load();
    }

    long long read_packet(unsigned char *packet, const size_t capacity, const int timeout_ms)
    {
#if defined(_WIN32)
        (void)packet;
        (void)capacity;
        (void)timeout_ms;
        fail("packet channel is unavailable on Windows");
        return -1;
#else
        if (packet == nullptr || capacity == 0 || capacity > kMaxPacketSize || timeout_ms < 0)
        {
            fail("invalid packet read arguments");
            return -1;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        int fd = -1;
        {
            std::unique_lock<std::mutex> lock(packet_mutex_);
            while (packet_fd_ < 0 && !finished_.load() && !stopping_.load())
            {
                if (packet_cv_.wait_until(lock, deadline) == std::cv_status::timeout)
                    return 0;
            }
            if (packet_fd_ < 0)
            {
                // Preserve the worker's earlier protocol/transport failure.
                // The packet reader often observes shutdown immediately after
                // that event and must not replace the actionable root cause
                // with this generic secondary symptom.
                if (last_error().empty())
                    fail("OpenVPN packet channel is closed");
                return -1;
            }
            fd = ::dup(packet_fd_);
        }
        if (fd < 0)
        {
            fail(std::string("duplicate packet channel: ") + std::strerror(errno));
            return -1;
        }

        const auto now = std::chrono::steady_clock::now();
        const int remaining = now >= deadline ? 0 : static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        pollfd descriptor{fd, POLLIN, 0};
        const int poll_result = ::poll(&descriptor, 1, remaining);
        if (poll_result == 0)
        {
            ::close(fd);
            return 0;
        }
        if (poll_result < 0)
        {
            const int saved_errno = errno;
            const std::string message = std::strerror(saved_errno);
            ::close(fd);
            if (saved_errno == EINTR)
                return 0;
            fail("poll packet channel: " + message);
            return -1;
        }

        const ssize_t count = ::recv(fd, packet, capacity, MSG_TRUNC);
        const int saved_errno = errno;
        ::close(fd);
        if (count <= 0)
        {
            fail(count == 0 ? "OpenVPN packet channel reached EOF"
                            : "read packet channel: " + std::string(std::strerror(saved_errno)));
            return -1;
        }
        if (static_cast<size_t>(count) > capacity)
        {
            fail("OpenVPN packet exceeds caller capacity");
            return -1;
        }
        return static_cast<long long>(count);
#endif
    }

    int write_packet(const unsigned char *packet, const size_t length)
    {
#if defined(_WIN32)
        (void)packet;
        (void)length;
        return fail("packet channel is unavailable on Windows");
#else
        if (packet == nullptr || length == 0 || length > kMaxPacketSize)
            return fail("invalid packet write arguments");
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(packet_mutex_);
            if (packet_fd_ >= 0)
                fd = ::dup(packet_fd_);
        }
        if (fd < 0)
            return fail("OpenVPN data channel is not ready");
        const ssize_t count = ::send(fd, packet, length, 0);
        const int saved_errno = errno;
        ::close(fd);
        if (count != static_cast<ssize_t>(length))
            return fail("write packet channel: " + std::string(std::strerror(saved_errno)));
        return 0;
#endif
    }

    std::string last_error() const
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return last_error_;
    }

    bool pause_on_connection_timeout() override
    {
        return false;
    }

    void event(const openvpn::ClientAPI::Event &event) override
    {
        if (event.name == "CONNECTED")
            ready_.store(true);
        else if (event.name == "DISCONNECTED" || event.name == "RECONNECTING" || event.fatal)
            ready_.store(false);

        const std::string info = bounded_text(event.info);
        if (event.error || event.fatal)
            set_error(event.name + (info.empty() ? "" : ": " + info));
        if (options_.event != nullptr)
            options_.event(options_.user_data, event.name.c_str(), info.c_str(),
                           (event.error || event.fatal) ? 1 : 0);
    }

    void acc_event(const openvpn::ClientAPI::AppCustomControlMessageEvent &) override
    {
    }

    void log(const openvpn::ClientAPI::LogInfo &) override
    {
        // Intentionally suppressed; see the logging boundary comment above.
    }

    void external_pki_cert_request(openvpn::ClientAPI::ExternalPKICertRequest &request) override
    {
        request.error = true;
        request.errorText = "external PKI is not supported";
    }

    void external_pki_sign_request(openvpn::ClientAPI::ExternalPKISignRequest &request) override
    {
        request.error = true;
        request.errorText = "external PKI is not supported";
    }

    bool socket_protect(openvpn_io::detail::socket_type, std::string, bool) override
    {
        // The only sockets opened by the MPL engine connect to 127.0.0.1.
        return true;
    }

    openvpn::TransportClientFactory *new_transport_factory(
        const openvpn::ExternalTransport::Config &config) override
    {
        std::string remote_host;
        std::string remote_port_text;
        openvpn::Protocol remote_protocol;
        config.remote_list->endpoint_available(&remote_host, &remote_port_text, &remote_protocol);
        if (!remote_protocol.is_tcp() && !remote_protocol.is_udp())
            throw std::runtime_error("external bridge supports only TCP and UDP OpenVPN transports");

        const unsigned long parsed_port = std::stoul(remote_port_text);
        if (parsed_port == 0 || parsed_port > 65535)
            throw std::runtime_error("OpenVPN remote port is out of range");

        uint64_t bridge_id = 0;
        uint16_t loopback_port = 0;
        char callback_error[512]{};
        const char *protocol = remote_protocol.is_udp() ? "udp" : "tcp";
        const int result = options_.open_bridge(
            options_.user_data,
            protocol,
            remote_host.c_str(),
            static_cast<uint16_t>(parsed_port),
            &bridge_id,
            &loopback_port,
            callback_error,
            sizeof(callback_error));
        if (result != 0 || bridge_id == 0 || loopback_port == 0)
        {
            const size_t length = strnlen(callback_error, sizeof(callback_error));
            const std::string detail(callback_error, length);
            throw std::runtime_error("detour bridge failed closed" +
                                     (detail.empty() ? std::string() : ": " + bounded_text(detail)));
        }

        auto lease = std::make_shared<BridgeLease>(options_, bridge_id);
        {
            std::lock_guard<std::mutex> lock(bridge_mutex_);
            if (stopping_.load())
            {
                lease->close();
                throw std::runtime_error("client stopped while opening detour bridge");
            }
            bridges_.push_back(lease);
        }

        const openvpn::Protocol local_protocol(remote_protocol.is_udp()
                                                   ? openvpn::Protocol::UDPv4
                                                   : openvpn::Protocol::TCPv4);
        openvpn::RemoteList::Ptr local_remote(new openvpn::RemoteList(
            "127.0.0.1", std::to_string(loopback_port), local_protocol, "ccore detour bridge"));

        openvpn::TransportClientFactory::Ptr delegate;
        if (remote_protocol.is_udp())
        {
            openvpn::UDPTransport::ClientConfig::Ptr udp = openvpn::UDPTransport::ClientConfig::new_obj();
            udp->remote_list = local_remote;
            udp->frame = config.frame;
            udp->stats = config.stats;
            udp->socket_protect = nullptr;
            udp->server_addr_float = false;
            udp->synchronous_dns_lookup = true;
            delegate = udp;
        }
        else
        {
            openvpn::TCPTransport::ClientConfig::Ptr tcp = openvpn::TCPTransport::ClientConfig::new_obj();
            tcp->remote_list = local_remote;
            tcp->frame = config.frame;
            tcp->stats = config.stats;
            tcp->socket_protect = nullptr;
            delegate = tcp;
        }
        return new BridgeTransportFactory(std::move(delegate), std::move(lease));
    }

    bool tun_builder_new() override
    {
        tunnel_state_ = TunnelState{};
        close_packet_fd();
        return true;
    }

    bool tun_builder_set_layer(const int layer) override
    {
        return layer == 0 || layer == 3;
    }

    bool tun_builder_set_remote_address(const std::string &, bool) override
    {
        return true;
    }

    bool tun_builder_add_address(const std::string &address,
                                 const int prefix_length,
                                 const std::string &,
                                 bool,
                                 bool) override
    {
        append_unique(tunnel_state_.local_addresses, address + "/" + std::to_string(prefix_length));
        return true;
    }

    bool tun_builder_set_route_metric_default(int) override
    {
        return true;
    }

    bool tun_builder_reroute_gw(const bool ipv4, const bool ipv6, unsigned int) override
    {
        if (ipv4)
            append_unique(tunnel_state_.routes, "0.0.0.0/0");
        if (ipv6)
            append_unique(tunnel_state_.routes, "::/0");
        return true;
    }

    bool tun_builder_add_route(const std::string &address,
                               const int prefix_length,
                               int,
                               bool) override
    {
        append_unique(tunnel_state_.routes, address + "/" + std::to_string(prefix_length));
        return true;
    }

    bool tun_builder_exclude_route(const std::string &, int, int, bool) override
    {
        // The private per-outbound netstack has no direct route to fall back to.
        return true;
    }

    bool tun_builder_set_dns_options(const openvpn::DnsOptions &dns) override
    {
        for (const auto &[priority, server] : dns.servers)
        {
            (void)priority;
            for (const auto &address : server.addresses)
                append_unique(tunnel_state_.dns_servers, address.address);
        }
        return true;
    }

    bool tun_builder_set_mtu(const int mtu) override
    {
        tunnel_state_.mtu = mtu;
        return mtu > 0;
    }

    bool tun_builder_set_session_name(const std::string &) override
    {
        return true;
    }

    bool tun_builder_set_allow_family(int, bool) override
    {
        return true;
    }

    bool tun_builder_set_allow_local_dns(bool) override
    {
        return true;
    }

    bool tun_builder_persist() override
    {
        return true;
    }

    int tun_builder_establish() override
    {
#if defined(_WIN32)
        fail("packet channel is unavailable on Windows");
        return -1;
#else
        if (tunnel_state_.mtu <= 0 || tunnel_state_.local_addresses.empty())
        {
            fail("server did not provide a usable tunnel address and MTU");
            return -1;
        }
        const std::string configuration = tunnel_json(tunnel_state_);
        if (options_.tunnel_config(options_.user_data, configuration.c_str()) != 0)
        {
            fail("host rejected the pushed tunnel configuration");
            return -1;
        }

        int sockets[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) != 0)
        {
            fail("create packet channel: " + std::string(std::strerror(errno)));
            return -1;
        }
        for (const int fd : sockets)
        {
            const int flags = ::fcntl(fd, F_GETFD);
            if (flags >= 0)
                ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
            int buffer_size = 1 << 20;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
        }
        {
            std::lock_guard<std::mutex> lock(packet_mutex_);
            packet_fd_ = sockets[0];
        }
        packet_cv_.notify_all();
        return sockets[1];
#endif
    }

    void tun_builder_establish_lite() override
    {
        packet_cv_.notify_all();
    }

    void tun_builder_teardown(bool) override
    {
        ready_.store(false);
    }

  private:
    int fail(const std::string &message)
    {
        set_error(message);
        return -1;
    }

    void set_error(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = bounded_text(message);
    }

    void run()
    {
        try
        {
            const openvpn::ClientAPI::Status status = connect();
            if (status.error && !stopping_.load())
                set_error((status.status.empty() ? std::string("OpenVPN connection failed") : status.status) +
                          (status.message.empty() ? std::string() : ": " + status.message));
        }
        catch (const std::exception &error)
        {
            if (!stopping_.load())
                set_error(std::string("OpenVPN connection exception: ") + error.what());
        }
        catch (...)
        {
            if (!stopping_.load())
                set_error("OpenVPN connection exception: unknown failure");
        }
        ready_.store(false);
        finished_.store(true);
        close_packet_fd();
        close_bridges();
        packet_cv_.notify_all();
    }

    void close_packet_fd()
    {
#if !defined(_WIN32)
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(packet_mutex_);
            fd = packet_fd_;
            packet_fd_ = -1;
        }
        if (fd >= 0)
            ::close(fd);
#endif
        packet_cv_.notify_all();
    }

    void close_bridges()
    {
        std::vector<std::shared_ptr<BridgeLease>> bridges;
        {
            std::lock_guard<std::mutex> lock(bridge_mutex_);
            bridges.swap(bridges_);
        }
        for (const auto &bridge : bridges)
            bridge->close();
    }

    ccore_ovpn3_client_options options_;
    std::string profile_;
    std::string username_;
    std::string password_;

    std::atomic<bool> configured_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{true};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> ready_{false};
    std::thread worker_;

    mutable std::mutex error_mutex_;
    std::string last_error_;

    std::mutex bridge_mutex_;
    std::vector<std::shared_ptr<BridgeLease>> bridges_;

    std::mutex packet_mutex_;
    std::condition_variable packet_cv_;
    int packet_fd_ = -1;
    TunnelState tunnel_state_;
};

} // namespace

struct ccore_ovpn3_client
{
    std::unique_ptr<RuntimeClient> implementation;
};

extern "C" int ccore_ovpn3_client_create(
    const ccore_ovpn3_client_options *options,
    ccore_ovpn3_client **client)
{
    if (client == nullptr)
        return -1;
    *client = nullptr;
    if (options == nullptr || options->struct_size < sizeof(ccore_ovpn3_client_options) ||
        options->profile == nullptr || options->open_bridge == nullptr ||
        options->close_bridge == nullptr || options->tunnel_config == nullptr)
        return -1;
    try
    {
        std::unique_ptr<ccore_ovpn3_client> wrapper(new ccore_ovpn3_client());
        wrapper->implementation.reset(new RuntimeClient(*options));
        const int result = wrapper->implementation->configure();
        *client = wrapper.release();
        return result;
    }
    catch (...)
    {
        return -1;
    }
}

extern "C" int ccore_ovpn3_client_start(ccore_ovpn3_client *client)
{
    return client == nullptr ? -1 : client->implementation->start_async();
}

extern "C" int ccore_ovpn3_client_ready(const ccore_ovpn3_client *client)
{
    return client != nullptr && client->implementation->ready() ? 1 : 0;
}

extern "C" int ccore_ovpn3_client_reconnect(ccore_ovpn3_client *client)
{
    return client == nullptr ? -1 : client->implementation->reconnect_now();
}

extern "C" int ccore_ovpn3_client_stop(ccore_ovpn3_client *client)
{
    return client == nullptr ? -1 : client->implementation->stop_and_join();
}

extern "C" void ccore_ovpn3_client_destroy(ccore_ovpn3_client *client)
{
    delete client;
}

extern "C" long long ccore_ovpn3_client_read_packet(
    ccore_ovpn3_client *client,
    unsigned char *packet,
    const size_t capacity,
    const int timeout_ms)
{
    return client == nullptr ? -1 : client->implementation->read_packet(packet, capacity, timeout_ms);
}

extern "C" int ccore_ovpn3_client_write_packet(
    ccore_ovpn3_client *client,
    const unsigned char *packet,
    const size_t length)
{
    return client == nullptr ? -1 : client->implementation->write_packet(packet, length);
}

extern "C" size_t ccore_ovpn3_client_last_error(
    const ccore_ovpn3_client *client,
    char *output,
    const size_t capacity)
{
    return copy_string(client == nullptr ? "OpenVPN client is null" : client->implementation->last_error(),
                       output,
                       capacity);
}
