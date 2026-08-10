#include "pulsegate/http/reverse_proxy.h"

#include <algorithm>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/write.hpp>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pulsegate::http {
namespace {

bool positive(std::chrono::milliseconds value) {
    return value > std::chrono::milliseconds::zero();
}

std::string methodName(HttpMethod method) {
    switch (method) {
        case HttpMethod::Get:
            return "GET";
        case HttpMethod::Head:
            return "HEAD";
        case HttpMethod::Post:
            return "POST";
        case HttpMethod::Put:
            return "PUT";
        case HttpMethod::Delete:
            return "DELETE";
        case HttpMethod::Options:
            return "OPTIONS";
        case HttpMethod::Unknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool hopByHop(const Header& header, const Headers& all) {
    static constexpr std::string_view fixed[] = {
        "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
        "te",         "trailer",    "transfer-encoding",  "upgrade"};
    if (std::find(std::begin(fixed), std::end(fixed), header.name) != std::end(fixed)) return true;
    for (const auto value : all.values("connection"))
        if (containsToken(value, header.name)) return true;
    return header.name == "host" || header.name == "content-length" ||
           header.name == "x-forwarded-for" || header.name == "x-forwarded-proto" ||
           header.name == "x-request-id";
}

std::string authority(const UpstreamEndpoint& upstream) {
    return upstream.host.find(':') == std::string::npos
               ? upstream.host + ":" + upstream.service
               : "[" + upstream.host + "]:" + upstream.service;
}

HttpResponse sanitizeUpstreamResponse(HttpResponse response) {
    HttpResponse sanitized;
    sanitized.status_code = response.status_code;
    sanitized.reason = std::move(response.reason);
    sanitized.body = std::move(response.body);
    sanitized.close_connection = response.close_connection;
    for (const auto& header : response.headers.entries()) {
        if (!hopByHop(header, response.headers)) {
            sanitized.headers.add(header.name, header.value);
        }
    }
    return sanitized;
}

std::string serializeStreamingHead(HttpResponse response, std::string_view request_id) {
    std::string bytes;
    bytes.append("HTTP/1.1 ");
    bytes.append(std::to_string(response.status_code));
    bytes.push_back(' ');
    bytes.append(response.reason);
    bytes.append("\r\n");
    for (const auto& header : response.headers.entries()) {
        if (header.name == "content-length" || header.name == "connection" ||
            header.name == "transfer-encoding") {
            continue;
        }
        bytes.append(header.name);
        bytes.append(": ");
        bytes.append(header.value);
        bytes.append("\r\n");
    }
    bytes.append("X-Request-Id: ");
    bytes.append(request_id);
    bytes.append("\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n");
    return bytes;
}

std::string chunkFrame(std::string_view bytes) {
    std::ostringstream output;
    output << std::hex << bytes.size() << "\r\n";
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output << "\r\n";
    return std::move(output).str();
}

ProxyResult streamedCloseResult(const UpstreamEndpoint& upstream) {
    HttpResponse response;
    response.close_connection = true;
    response.already_written = true;
    return {.error = ProxyError::None, .response = std::move(response), .upstream = upstream};
}

}  // namespace

std::string serializeUpstreamRequest(const HttpRequest& request, const UpstreamEndpoint& upstream,
                                     const RequestContext& context) {
    std::string bytes;
    bytes.reserve(256 + request.body.size());
    bytes.append(methodName(request.method));
    bytes.push_back(' ');
    bytes.append(request.target.empty() ? "/" : request.target);
    bytes.append(" HTTP/1.1\r\n");
    for (const auto& header : request.headers.entries()) {
        if (hopByHop(header, request.headers)) continue;
        bytes.append(header.name);
        bytes.append(": ");
        bytes.append(header.value);
        bytes.append("\r\n");
    }
    bytes.append("Host: ");
    bytes.append(authority(upstream));
    bytes.append("\r\n");
    bytes.append("X-Forwarded-For: ");
    bytes.append(context.peer.address().is_unspecified() ? "unknown"
                                                         : context.peer.address().to_string());
    bytes.append("\r\nX-Forwarded-Proto: http\r\nX-Request-Id: ");
    bytes.append(context.request_id);
    bytes.append("\r\nContent-Length: ");
    bytes.append(std::to_string(request.body.size()));
    // v0.6.0-a closes every upstream transaction. This makes response framing
    // and retry semantics explicit before stage 7 introduces a pool.
    bytes.append("\r\nConnection: close\r\n\r\n");
    bytes.append(request.body);
    return bytes;
}

HttpResponse makeProxyErrorResponse(ProxyError error) {
    HttpResponse response;
    response.close_connection = false;
    switch (error) {
        case ProxyError::DnsFailure:
        case ProxyError::ConnectFailure:
        case ProxyError::UpstreamProtocol:
        case ProxyError::UpstreamBodyTooLarge:
            response.status_code = 502;
            response.reason = "Bad Gateway";
            response.body = "bad gateway\n";
            break;
        case ProxyError::ConnectTimeout:
        case ProxyError::ResponseTimeout:
            response.status_code = 504;
            response.reason = "Gateway Timeout";
            response.body = "gateway timeout\n";
            break;
        case ProxyError::Cancelled:
            response.status_code = 503;
            response.reason = "Service Unavailable";
            response.body = "upstream transaction cancelled\n";
            break;
        case ProxyError::UnsupportedRequest:
            response.status_code = 501;
            response.reason = "Not Implemented";
            response.body = "upstream upgrade is not supported\n";
            break;
        case ProxyError::None:
            return response;
    }
    response.headers.add("Content-Type", "text/plain");
    return response;
}

ProxySession::ProxySession(net::asio::any_io_executor executor, UpstreamEndpoint upstream,
                           ProxyLimits limits)
    : executor_(std::move(executor)),
      upstream_(std::move(upstream)),
      limits_(limits),
      resolver_(executor_),
      socket_(executor_),
      phase_deadline_(std::make_shared<net::Deadline>(executor_)),
      total_deadline_(std::make_shared<net::Deadline>(executor_)),
      input_(limits_.read_chunk_bytes, limits_.max_buffer_bytes),
      parser_(limits_.response_parser) {
    if (upstream_.host.empty() || upstream_.service.empty() || limits_.read_chunk_bytes == 0 ||
        limits_.max_buffer_bytes < limits_.read_chunk_bytes || !positive(limits_.dns_timeout) ||
        !positive(limits_.connect_timeout) || !positive(limits_.response_timeout) ||
        !positive(limits_.total_timeout))
        throw std::invalid_argument("proxy configuration is outside safe bounds");
}

net::Awaitable<ProxyResult> ProxySession::execute(RequestContext& context, HttpRequest request) {
    [[maybe_unused]] const auto self = shared_from_this();
    const auto connection = request.headers.get("connection");
    if (request.headers.contains("upgrade") ||
        (connection.has_value() && containsToken(*connection, "upgrade"))) {
        co_return ProxyResult{
            .error = ProxyError::UnsupportedRequest, .response = {}, .upstream = upstream_};
    }
    total_deadline_->arm(limits_.total_timeout, [weak = weak_from_this()] {
        if (auto locked = weak.lock()) locked->cancelInExecutor(ProxyStopReason::TotalTimeout);
    });
    if (!co_await resolve() || !co_await connect()) {
        disarmTimers();
        co_return ProxyResult{.error = errorForStopReason(), .response = {}, .upstream = upstream_};
    }
    const auto bytes = serializeUpstreamRequest(request, upstream_, context);
    if (!co_await sendRequest(bytes)) {
        disarmTimers();
        co_return ProxyResult{.error = errorForStopReason(), .response = {}, .upstream = upstream_};
    }
    auto result = co_await readResponse(request.method == HttpMethod::Head);
    disarmTimers();
    net::ErrorCode ignored;
    socket_.shutdown(net::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    co_return result;
}

net::Awaitable<ProxyResult> ProxySession::forward(RequestContext& context, HttpRequest request) {
    [[maybe_unused]] const auto self = shared_from_this();
    const auto connection = request.headers.get("connection");
    if (request.headers.contains("upgrade") ||
        (connection.has_value() && containsToken(*connection, "upgrade"))) {
        co_return ProxyResult{
            .error = ProxyError::UnsupportedRequest, .response = {}, .upstream = upstream_};
    }
    if (!context.write_downstream) {
        co_return ProxyResult{
            .error = ProxyError::ConnectFailure, .response = {}, .upstream = upstream_};
    }
    total_deadline_->arm(limits_.total_timeout, [weak = weak_from_this()] {
        if (auto locked = weak.lock()) locked->cancelInExecutor(ProxyStopReason::TotalTimeout);
    });
    if (!co_await resolve() || !co_await connect()) {
        disarmTimers();
        co_return ProxyResult{.error = errorForStopReason(), .response = {}, .upstream = upstream_};
    }
    const auto bytes = serializeUpstreamRequest(request, upstream_, context);
    if (!co_await sendRequest(bytes)) {
        disarmTimers();
        co_return ProxyResult{.error = errorForStopReason(), .response = {}, .upstream = upstream_};
    }
    auto result = co_await forwardResponse(context);
    disarmTimers();
    net::ErrorCode ignored;
    socket_.shutdown(net::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    co_return result;
}

net::Awaitable<bool> ProxySession::resolve() {
    state_ = ProxyState::Resolving;
    armPhase(limits_.dns_timeout, ProxyStopReason::DnsTimeout);
    net::ErrorCode error;
    resolved_ = co_await resolver_.async_resolve(
        upstream_.host, upstream_.service, net::asio::redirect_error(net::use_awaitable, error));
    phase_deadline_->disarm();
    if (error || resolved_.empty()) {
        if (stop_reason_ == ProxyStopReason::None) stop_reason_ = ProxyStopReason::InternalError;
        co_return false;
    }
    co_return true;
}

net::Awaitable<bool> ProxySession::connect() {
    state_ = ProxyState::Connecting;
    armPhase(limits_.connect_timeout, ProxyStopReason::ConnectTimeout);
    net::ErrorCode error;
    co_await net::asio::async_connect(socket_, resolved_,
                                      net::asio::redirect_error(net::use_awaitable, error));
    phase_deadline_->disarm();
    if (error) {
        if (stop_reason_ == ProxyStopReason::None) stop_reason_ = ProxyStopReason::InternalError;
        co_return false;
    }
    co_return true;
}

net::Awaitable<bool> ProxySession::sendRequest(const std::string& bytes) {
    state_ = ProxyState::SendingRequest;
    armPhase(limits_.response_timeout, ProxyStopReason::ResponseTimeout);
    net::ErrorCode error;
    co_await net::asio::async_write(socket_, net::asio::buffer(bytes),
                                    net::asio::redirect_error(net::use_awaitable, error));
    phase_deadline_->disarm();
    if (error) {
        if (stop_reason_ == ProxyStopReason::None) stop_reason_ = ProxyStopReason::InternalError;
        co_return false;
    }
    co_return true;
}

net::Awaitable<ProxyResult> ProxySession::readResponse(bool head_request) {
    parser_.reset(head_request);
    state_ = ProxyState::ReadingResponseHeaders;
    for (;;) {
        const auto parsed = parser_.parse(input_);
        if (parsed == ResponseParseResult::Complete) {
            state_ = ProxyState::Complete;
            co_return ProxyResult{.error = ProxyError::None,
                                  .response = sanitizeUpstreamResponse(parser_.takeResponse()),
                                  .upstream = upstream_};
        }
        if (parsed != ResponseParseResult::NeedMore) {
            state_ = ProxyState::Failed;
            co_return ProxyResult{.error = parsed == ResponseParseResult::BodyTooLarge
                                               ? ProxyError::UpstreamBodyTooLarge
                                               : ProxyError::UpstreamProtocol,
                                  .response = {},
                                  .upstream = upstream_};
        }
        state_ = parser_.headerComplete() ? ProxyState::ReadingResponseBody
                                          : ProxyState::ReadingResponseHeaders;
        auto writable = input_.prepare(limits_.read_chunk_bytes);
        armPhase(limits_.response_timeout, ProxyStopReason::ResponseTimeout);
        net::ErrorCode error;
        const auto count =
            co_await socket_.async_read_some(net::asio::buffer(writable.data(), writable.size()),
                                             net::asio::redirect_error(net::use_awaitable, error));
        phase_deadline_->disarm();
        if (error || count == 0) {
            if (stop_reason_ != ProxyStopReason::None) {
                state_ = ProxyState::Cancelled;
                co_return ProxyResult{
                    .error = errorForStopReason(), .response = {}, .upstream = upstream_};
            }
            const auto eof = parser_.finishOnEof();
            if (eof == ResponseParseResult::Complete) {
                state_ = ProxyState::Complete;
                co_return ProxyResult{.error = ProxyError::None,
                                      .response = sanitizeUpstreamResponse(parser_.takeResponse()),
                                      .upstream = upstream_};
            }
            state_ = ProxyState::Failed;
            co_return ProxyResult{
                .error = ProxyError::UpstreamProtocol, .response = {}, .upstream = upstream_};
        }
        input_.commit(count);
    }
}

net::Awaitable<ProxyResult> ProxySession::forwardResponse(RequestContext& context) {
    parser_.reset(false);
    state_ = ProxyState::ReadingResponseHeaders;
    bool response_started = false;
    std::size_t forwarded_body_bytes = 0;
    for (;;) {
        const auto parsed = parser_.parse(input_);
        if (parser_.headerComplete() && !response_started) {
            const auto header = sanitizeUpstreamResponse(parser_.response());
            if (!co_await context.write_downstream(
                    serializeStreamingHead(header, context.request_id))) {
                cancelInExecutor(ProxyStopReason::DownstreamClosed);
                co_return streamedCloseResult(upstream_);
            }
            response_started = true;
        }
        const auto& body = parser_.response().body;
        if (body.size() > forwarded_body_bytes) {
            const auto bytes = std::string_view(body).substr(forwarded_body_bytes);
            if (!co_await context.write_downstream(chunkFrame(bytes))) {
                cancelInExecutor(ProxyStopReason::DownstreamClosed);
                co_return streamedCloseResult(upstream_);
            }
            forwarded_body_bytes = body.size();
        }
        if (parsed == ResponseParseResult::Complete) {
            if (!co_await context.write_downstream("0\r\n\r\n")) {
                cancelInExecutor(ProxyStopReason::DownstreamClosed);
            }
            state_ = ProxyState::Complete;
            HttpResponse completed;
            completed.already_written = true;
            co_return ProxyResult{
                .error = ProxyError::None, .response = std::move(completed), .upstream = upstream_};
        }
        if (parsed != ResponseParseResult::NeedMore) {
            state_ = ProxyState::Failed;
            co_return response_started
                ? streamedCloseResult(upstream_)
                : ProxyResult{.error = parsed == ResponseParseResult::BodyTooLarge
                                           ? ProxyError::UpstreamBodyTooLarge
                                           : ProxyError::UpstreamProtocol,
                              .response = {},
                              .upstream = upstream_};
        }
        state_ = parser_.headerComplete() ? ProxyState::ReadingResponseBody
                                          : ProxyState::ReadingResponseHeaders;
        auto writable = input_.prepare(limits_.read_chunk_bytes);
        armPhase(limits_.response_timeout, ProxyStopReason::ResponseTimeout);
        net::ErrorCode error;
        const auto count =
            co_await socket_.async_read_some(net::asio::buffer(writable.data(), writable.size()),
                                             net::asio::redirect_error(net::use_awaitable, error));
        phase_deadline_->disarm();
        if (error || count == 0) {
            if (stop_reason_ != ProxyStopReason::None) {
                state_ = ProxyState::Cancelled;
                co_return response_started
                    ? streamedCloseResult(upstream_)
                    : ProxyResult{
                          .error = errorForStopReason(), .response = {}, .upstream = upstream_};
            }
            const auto eof = parser_.finishOnEof();
            if (eof == ResponseParseResult::Complete) {
                continue;
            }
            state_ = ProxyState::Failed;
            co_return response_started
                ? streamedCloseResult(upstream_)
                : ProxyResult{
                      .error = ProxyError::UpstreamProtocol, .response = {}, .upstream = upstream_};
        }
        input_.commit(count);
    }
}

void ProxySession::cancel(ProxyStopReason reason) {
    auto self = shared_from_this();
    net::asio::dispatch(executor_, [self, reason] { self->cancelInExecutor(reason); });
}
ProxyState ProxySession::state() const noexcept {
    return state_;
}
ProxyStopReason ProxySession::stopReason() const noexcept {
    return stop_reason_;
}
void ProxySession::armPhase(std::chrono::milliseconds timeout, ProxyStopReason reason) {
    phase_deadline_->arm(timeout, [weak = weak_from_this(), reason] {
        if (auto self = weak.lock()) self->cancelInExecutor(reason);
    });
}
void ProxySession::disarmTimers() {
    phase_deadline_->disarm();
    total_deadline_->disarm();
}
void ProxySession::cancelInExecutor(ProxyStopReason reason) {
    if (stop_reason_ != ProxyStopReason::None) return;
    stop_reason_ = reason;
    state_ = ProxyState::Cancelled;
    resolver_.cancel();
    net::ErrorCode ignored;
    socket_.cancel(ignored);
    socket_.close(ignored);
}
ProxyError ProxySession::errorForStopReason() const noexcept {
    switch (stop_reason_) {
        case ProxyStopReason::DnsTimeout:
            return ProxyError::ConnectTimeout;
        case ProxyStopReason::ConnectTimeout:
            return ProxyError::ConnectTimeout;
        case ProxyStopReason::ResponseTimeout:
        case ProxyStopReason::TotalTimeout:
            return ProxyError::ResponseTimeout;
        case ProxyStopReason::DownstreamClosed:
        case ProxyStopReason::ServerShutdown:
            return ProxyError::Cancelled;
        case ProxyStopReason::None:
        case ProxyStopReason::InternalError:
            return state_ == ProxyState::Resolving ? ProxyError::DnsFailure
                                                   : ProxyError::ConnectFailure;
    }
    return ProxyError::ConnectFailure;
}

ReverseProxy::ReverseProxy(std::vector<UpstreamEndpoint> upstreams, ProxyLimits limits)
    : state_(std::make_shared<State>(std::move(upstreams), std::move(limits))) {
    if (state_->upstreams.empty())
        throw std::invalid_argument("reverse proxy requires an upstream");
    for (const auto& upstream : state_->upstreams)
        if (upstream.host.empty() || upstream.service.empty())
            throw std::invalid_argument("upstream endpoint requires host and service");
}

net::Awaitable<HttpResponse> ReverseProxy::operator()(RequestContext& context,
                                                      HttpRequest request) const {
    const auto index =
        state_->next.fetch_add(1, std::memory_order_relaxed) % state_->upstreams.size();
    auto session =
        std::make_shared<ProxySession>(context.executor, state_->upstreams[index], state_->limits);
    if (context.set_current_proxy) {
        context.set_current_proxy(session);
    }
    const auto result = request.method == HttpMethod::Head
                            ? co_await session->execute(context, std::move(request))
                            : co_await session->forward(context, std::move(request));
    if (context.set_current_proxy) {
        context.set_current_proxy({});
    }
    co_return result.error == ProxyError::None ? result.response
                                               : makeProxyErrorResponse(result.error);
}

}  // namespace pulsegate::http
