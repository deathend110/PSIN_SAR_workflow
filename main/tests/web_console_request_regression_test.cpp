#include "workflow/web/web_console_protocol.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
    using workflow::web::detail::HttpRequestError;
    using workflow::web::detail::HttpRequestErrorKind;
    using workflow::web::detail::HttpRequestLimits;
    using workflow::web::detail::MapHttpRequestErrorCode;
    using workflow::web::detail::MapHttpRequestStatus;
    using workflow::web::detail::ParseHttpRequestHeaderBlock;

    void Fail(const std::string &message)
    {
        std::cerr << "web_console_request_regression_test failed: " << message << '\n';
        std::exit(1);
    }

    void Expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            Fail(message);
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

    void ExpectRequestError(void (*fn)(),
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
            Expect(error.kind() == expected_kind, "unexpected error kind");
            Expect(MapHttpRequestStatus(error.kind()) == expected_status, "unexpected HTTP status mapping");
            Expect(MapHttpRequestErrorCode(error.kind()) == expected_code, "unexpected error code mapping");
        }
    }

    void ThrowInvalidContentLength()
    {
        ParseHttpRequestHeaderBlock("POST /api/settings HTTP/1.1\r\nContent-Length: nope\r\n",
                                    HttpRequestLimits{1024, 1024, 1000});
    }

    void ThrowOversizedHeader()
    {
        ParseHttpRequestHeaderBlock("GET / HTTP/1.1\r\nX-Test: 12345\r\n",
                                    HttpRequestLimits{16, 1024, 1000});
    }

    void ThrowOversizedBody()
    {
        ParseHttpRequestHeaderBlock("POST /api/settings HTTP/1.1\r\nContent-Length: 33\r\n",
                                    HttpRequestLimits{1024, 32, 1000});
    }

    void TestInvalidContentLengthMapsTo400()
    {
        ExpectRequestError(&ThrowInvalidContentLength,
                           HttpRequestErrorKind::BadRequest,
                           "400 Bad Request",
                           "invalid_request");
    }

    void TestOversizedHeaderMapsTo413()
    {
        ExpectRequestError(&ThrowOversizedHeader,
                           HttpRequestErrorKind::PayloadTooLarge,
                           "413 Payload Too Large",
                           "request_too_large");
    }

    void TestOversizedBodyMapsTo413()
    {
        ExpectRequestError(&ThrowOversizedBody,
                           HttpRequestErrorKind::PayloadTooLarge,
                           "413 Payload Too Large",
                           "request_too_large");
    }

    void TestTimeoutMapsTo408()
    {
        Expect(MapHttpRequestStatus(HttpRequestErrorKind::Timeout) == "408 Request Timeout",
               "timeout should map to 408 Request Timeout");
        Expect(MapHttpRequestErrorCode(HttpRequestErrorKind::Timeout) == "request_timeout",
               "timeout should map to request_timeout");
    }

    void TestProtocolReadPathStillUsesSelectTimeout()
    {
        const auto source_path = FindRepoFile("main/src/web_console_protocol.cpp");
        const std::string source = ReadFileText(source_path);

        Expect(source.find("ReadHttpRequestFromSocket(") != std::string::npos,
               "protocol request read path should stay in a named helper");
        Expect(source.find("::select(") != std::string::npos,
               "protocol request read path should use select() for timeout enforcement");
    }
}

int main()
{
    TestInvalidContentLengthMapsTo400();
    TestOversizedHeaderMapsTo413();
    TestOversizedBodyMapsTo413();
    TestTimeoutMapsTo408();
    TestProtocolReadPathStillUsesSelectTimeout();
    std::cout << "web_console_request_regression_test passed\n";
    return 0;
}
