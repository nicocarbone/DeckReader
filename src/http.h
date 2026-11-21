// HTTP(S) requests through libcurl (from the Freedesktop runtime), for the
// wallabag articles (D31). Blocking: called from the articles thread only,
// never from the main loop. Redirects are followed only when asked
// (pictures): for the API a POST would lose its body, and the user is told
// the address to use instead. Only http and https; every response is
// size-limited.
#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace dr {

struct HttpRequest {
    std::string method = "GET"; // GET, POST, PATCH
    std::string url;
    std::string body;           // sent as is (POST, PATCH)
    std::string content_type;   // of the body
    std::vector<std::string> headers; // "Name: value"
    size_t max_bytes = 16u << 20;     // a longer response is an error
    long timeout_s = 60;              // whole request
    long connect_timeout_s = 20;
    // Given up when under 1 KB/s for this long (0 = no limit): a stalled
    // site doesn't hold the sync for the whole timeout.
    long low_speed_s = 0;
    bool follow_redirects = false;    // pictures only (GET, no credentials)
    // Addresses from web pages (pictures): no connection to this device,
    // the local network or link-local addresses, redirects included (D31).
    bool public_only = false;
};

struct HttpResponse {
    long status = 0;       // HTTP status; 0 when no answer came
    std::string body;
    std::string location;  // Location header (redirects)
    std::string error;     // transport problem for the user, "" when an answer came
    bool cancelled = false;
    bool too_large = false;   // longer than max_bytes (`error` says so too)
    bool unreachable = false; // the host: not found, refused, too slow, TLS failed
};

// Once per process, from the main thread, before any request.
void http_global_init();
void http_global_cleanup();

// Requests made one after another through one client reuse its open
// connections (an article's pictures, mostly from one site, then the next
// article from wallabag). One client per thread.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    // Blocks until done, failed or `cancel` becomes true (checked about
    // once a second, also while connecting).
    HttpResponse request(const HttpRequest& req, const std::atomic<bool>* cancel = nullptr);

private:
    void* curl_; // CURL*
};

// One request with a client of its own.
HttpResponse http_request(const HttpRequest& req, const std::atomic<bool>* cancel = nullptr);

// application/x-www-form-urlencoded
std::string url_encode(const std::string& s);
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields);

// "https://host:port/path" -> "host:port" (for messages).
std::string url_host(const std::string& url);
// "host:port", "[::1]:8080" -> "host", "::1", lower case.
std::string host_name(const std::string& host);

// A numeric IPv4 or IPv6 address that is on the internet: false for
// loopback, private (RFC 1918, fc00::/7), shared (100.64/10), link-local,
// multicast, unspecified and reserved ones, and IPv4-mapped forms of them.
bool public_ip(const std::string& ip);
// A numeric IPv4 or IPv6 address (not a name).
bool is_ip(const std::string& s);

} // namespace dr
