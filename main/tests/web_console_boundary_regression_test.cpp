#include "workflow/web/web_console_protocol.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using workflow::web::detail::HttpRequestError;
    using workflow::web::detail::HttpRequestErrorKind;
    using workflow::web::detail::HttpRequestLimits;
    using workflow::web::detail::MapHttpRequestErrorCode;
    using workflow::web::detail::MapHttpRequestStatus;
    using workflow::web::detail::MatchWebConsoleRoute;
    using workflow::web::detail::ParseContentLengthValue;
    using workflow::web::detail::ParseHttpRequestHeaderBlock;
    using workflow::web::detail::WebConsoleRoute;

    void Fail(const std::string &message)
    {
        std::cerr << "web_console_boundary_regression_test failed: " << message << '\n';
        std::exit(1);
    }

    void Expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            Fail(message);
        }
    }

    void ExpectContains(const std::string &text, const std::string &needle, const std::string &message)
    {
        if (text.find(needle) == std::string::npos)
        {
            Fail(message + " needle=" + needle);
        }
    }

    std::filesystem::path FindRepoFile(const std::filesystem::path &relative_path)
    {
        std::filesystem::path current = std::filesystem::current_path();
        for (int depth = 0; depth < 8; ++depth)
        {
            const std::filesystem::path candidate = current / relative_path;
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }

            if (!current.has_parent_path())
            {
                break;
            }
            current = current.parent_path();
        }

        Fail("could not locate " + relative_path.generic_string());
        return {};
    }

    std::string ReadFileText(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        Expect(static_cast<bool>(input), "unable to open " + path.generic_string());

        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    template <typename Fn>
    void ExpectHttpRequestError(Fn &&fn,
                                HttpRequestErrorKind expected_kind,
                                const std::string &expected_status,
                                const std::string &expected_code)
    {
        try
        {
            fn();
            Fail("expected HttpRequestError");
        }
        catch (const HttpRequestError &error)
        {
            Expect(error.kind() == expected_kind, "unexpected HttpRequestError kind");
            Expect(MapHttpRequestStatus(error.kind()) == expected_status, "unexpected HTTP status mapping");
            Expect(MapHttpRequestErrorCode(error.kind()) == expected_code, "unexpected HTTP error code mapping");
        }
    }

    void TestHttpParserBoundaries()
    {
        const std::string exact_header =
            "GET /api/sources?workflow=rd HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "X-Trace: abc\r\n";
        const auto exact_limits = HttpRequestLimits{exact_header.size(), 1024, 1000};
        const auto request = ParseHttpRequestHeaderBlock(exact_header, exact_limits);

        Expect(request.method == "GET", "request method should be parsed");
        Expect(request.path == "/api/sources", "request path should be split from query");
        Expect(request.query == "workflow=rd", "request query should be preserved");
        Expect(request.headers.at("host") == "localhost", "header lookup should be case-insensitive");
        Expect(request.headers.at("x-trace") == "abc", "second header should be preserved");
        Expect(request.content_length == 0, "missing Content-Length should default to zero");

        const std::string post_header =
            "POST /api/settings HTTP/1.1\r\n"
            "Content-Length: 512\r\n";
        const auto post_request = ParseHttpRequestHeaderBlock(post_header, HttpRequestLimits{1024, 1024, 1000});
        Expect(post_request.content_length == 512, "Content-Length should be parsed");

        Expect(ParseContentLengthValue("1024", HttpRequestLimits{1024, 1024, 1000}) == 1024,
               "exact body-size boundary should be accepted");

        ExpectHttpRequestError([&]() {
            ParseHttpRequestHeaderBlock("POST /api/settings HTTP/1.1\r\nContent-Length: nope\r\n",
                                        HttpRequestLimits{1024, 1024, 1000});
        },
                               HttpRequestErrorKind::BadRequest,
                               "400 Bad Request",
                               "invalid_request");

        ExpectHttpRequestError([&]() {
            ParseHttpRequestHeaderBlock(exact_header + "X",
                                        HttpRequestLimits{exact_header.size(), 1024, 1000});
        },
                               HttpRequestErrorKind::PayloadTooLarge,
                               "413 Payload Too Large",
                               "request_too_large");

        ExpectHttpRequestError([&]() {
            ParseContentLengthValue("1025", HttpRequestLimits{1024, 1024, 1000});
        },
                               HttpRequestErrorKind::PayloadTooLarge,
                               "413 Payload Too Large",
                               "request_too_large");
    }

    void TestRouteMatchingIsExplicitAndDirectlyTestable()
    {
        workflow::web::detail::HttpRequest request;
        request.method = "GET";
        request.path = "/api/state";
        Expect(MatchWebConsoleRoute(request) == WebConsoleRoute::ApiState,
               "GET /api/state should map to ApiState");

        request.path = "/api/settings";
        Expect(MatchWebConsoleRoute(request) == WebConsoleRoute::ApiSettingsGet,
               "GET /api/settings should map to ApiSettingsGet");

        request.path = "/api/sources";
        Expect(MatchWebConsoleRoute(request) == WebConsoleRoute::ApiSources,
               "GET /api/sources should map to ApiSources");

        request.path = "/events";
        Expect(MatchWebConsoleRoute(request) == WebConsoleRoute::Events,
               "GET /events should map to Events");

        request.method = "POST";
        request.path = "/api/manual/key";
        Expect(MatchWebConsoleRoute(request) == WebConsoleRoute::ApiManualKey,
               "POST /api/manual/key should map to ApiManualKey");

        request.path = "/api/unknown";
        Expect(MatchWebConsoleRoute(request) == WebConsoleRoute::Unknown,
               "unknown route should stay observable as Unknown");

        const std::string protocol_source = ReadFileText(FindRepoFile("main/src/web_console_protocol.cpp"));
        ExpectContains(protocol_source, "WebConsoleRoute MatchWebConsoleRoute(const HttpRequest &request)",
                       "route matching should stay in a dedicated protocol helper");
        ExpectContains(protocol_source, "if (is_get && request.path == \"/api/state\")",
                       "state route should remain explicit inside the matcher");
        ExpectContains(protocol_source, "if (is_post && request.path == \"/api/manual/key\")",
                       "manual key route should remain explicit inside the matcher");
    }

    void TestSseAbstractionRemainsObservable()
    {
        const std::string source = ReadFileText(FindRepoFile("main/src/web_console_server.cpp"));

        ExpectContains(source, "bool WebConsoleServer::handleSseClient(int client_fd)",
                       "SSE client handling should stay in its own helper");
        ExpectContains(source, "controller_.setEventCallback([this]",
                       "server should still register a queueing callback");
        ExpectContains(source, "class SseHub",
                       "SSE client management should be extracted into SseHub");
        ExpectContains(source, "dispatchRoute(",
                       "route dispatch should remain a named server helper");
        ExpectContains(source, "detail::ReadHttpRequestFromSocket(client_fd)",
                       "server should delegate socket request reading to protocol helper");
        ExpectContains(source, "queueEvent(event_name, payload)",
                       "event queueing should remain distinct from route dispatch");
        ExpectContains(source, "sendHeartbeatFrames()",
                       "SSE heartbeat emission should remain visible");
        ExpectContains(source, "sse_hub_->acceptClient",
                       "server should delegate SSE client registration through the hub");
    }
}

int main()
{
    TestHttpParserBoundaries();
    TestRouteMatchingIsExplicitAndDirectlyTestable();
    TestSseAbstractionRemainsObservable();
    std::cout << "web_console_boundary_regression_test passed\n";
    return 0;
}
