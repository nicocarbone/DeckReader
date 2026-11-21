#include "http.h"

#include "util.h"
#include "version.h"

#include <arpa/inet.h>
#include <cctype>
#include <cstring>
#include <curl/curl.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace dr {

void http_global_init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_global_cleanup() {
    curl_global_cleanup();
}

namespace {

struct Transfer {
    HttpResponse* res;
    size_t max_bytes;
    bool too_large = false;
    const std::atomic<bool>* cancel;
    bool public_only = false;
    bool refused = false; // an address public_only turned down
};

bool public_ipv4(const unsigned char* a) {
    const unsigned b0 = a[0], b1 = a[1];
    return !(b0 == 0 || b0 == 10 || b0 == 127 ||           // this network, private, loopback
             (b0 == 100 && (b1 & 0xC0) == 64) ||          // shared (carrier NAT)
             (b0 == 169 && b1 == 254) ||                  // link-local
             (b0 == 172 && (b1 & 0xF0) == 16) ||          // private
             (b0 == 192 && b1 == 168) ||                  // private
             (b0 == 192 && b1 == 0 && a[2] == 0) ||       // protocol assignments
             (b0 == 198 && (b1 & 0xFE) == 18) ||          // benchmarking
             b0 >= 224);                                  // multicast, reserved, broadcast
}

bool public_ipv6(const unsigned char* a) {
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    static const unsigned char zeros[12] = {0};
    if (std::memcmp(a, mapped, 12) == 0)
        return public_ipv4(a + 12);
    if (std::memcmp(a, zeros, 12) == 0)
        return false; // ::, ::1 and the old IPv4-compatible forms
    return !(a[0] == 0xFF ||                   // multicast
             (a[0] & 0xFE) == 0xFC ||          // unique local
             (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) ||  // link-local
             (a[0] == 0xFE && (a[1] & 0xC0) == 0xC0));   // site-local (deprecated)
}

// Every connection is made here, for redirects too, after the name was
// looked up: what the address really is counts, not the name in the page.
curl_socket_t on_open_socket(void* user, curlsocktype, curl_sockaddr* addr) {
    Transfer* t = static_cast<Transfer*>(user);
    if (t->public_only) {
        bool ok = false;
        if (addr->family == AF_INET && addr->addrlen >= sizeof(sockaddr_in))
            ok = public_ipv4(reinterpret_cast<const unsigned char*>(
                &reinterpret_cast<const sockaddr_in*>(&addr->addr)->sin_addr));
        else if (addr->family == AF_INET6 && addr->addrlen >= sizeof(sockaddr_in6))
            ok = public_ipv6(reinterpret_cast<const sockaddr_in6*>(&addr->addr)->sin6_addr.s6_addr);
        if (!ok) {
            t->refused = true;
            return CURL_SOCKET_BAD; // curl tries the next address, if any
        }
    }
    return ::socket(addr->family, addr->socktype | SOCK_CLOEXEC, addr->protocol);
}

size_t on_body(char* data, size_t size, size_t n, void* user) {
    Transfer* t = static_cast<Transfer*>(user);
    size_t len = size * n;
    if (t->res->body.size() + len > t->max_bytes) {
        t->too_large = true;
        return 0; // aborts with CURLE_WRITE_ERROR
    }
    t->res->body.append(data, len);
    return len;
}

size_t on_header(char* data, size_t size, size_t n, void* user) {
    Transfer* t = static_cast<Transfer*>(user);
    size_t len = size * n;
    std::string line(data, len);
    if (starts_with(line, "HTTP/"))
        t->res->location.clear(); // a new response (after 100 Continue)
    else if (to_lower(line.substr(0, 9)) == "location:")
        t->res->location = trim(line.substr(9));
    return len;
}

int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const Transfer* t = static_cast<const Transfer*>(user);
    return t->cancel && t->cancel->load() ? 1 : 0;
}

} // namespace

std::string url_host(const std::string& url) {
    std::string s = url;
    size_t p = s.find("://");
    if (p != std::string::npos)
        s = s.substr(p + 3);
    s = s.substr(0, s.find_first_of("/?#"));
    size_t at = s.rfind('@');
    if (at != std::string::npos)
        s = s.substr(at + 1);
    return s;
}

std::string host_name(const std::string& host) {
    std::string h = to_lower(host);
    if (!h.empty() && h[0] == '[') {
        size_t end = h.find(']');
        return end == std::string::npos ? h.substr(1) : h.substr(1, end - 1);
    }
    return h.substr(0, h.find(':'));
}

bool public_ip(const std::string& ip) {
    unsigned char a[16];
    if (inet_pton(AF_INET, ip.c_str(), a) == 1)
        return public_ipv4(a);
    if (inet_pton(AF_INET6, ip.c_str(), a) == 1)
        return public_ipv6(a);
    return false;
}

bool is_ip(const std::string& s) {
    unsigned char a[16];
    return inet_pton(AF_INET, s.c_str(), a) == 1 || inet_pton(AF_INET6, s.c_str(), a) == 1;
}

HttpClient::HttpClient() : curl_(curl_easy_init()) {}

HttpClient::~HttpClient() {
    if (curl_)
        curl_easy_cleanup(static_cast<CURL*>(curl_));
}

HttpResponse http_request(const HttpRequest& req, const std::atomic<bool>* cancel) {
    HttpClient client;
    return client.request(req, cancel);
}

HttpResponse HttpClient::request(const HttpRequest& req, const std::atomic<bool>* cancel) {
    HttpResponse res;
    CURL* c = static_cast<CURL*>(curl_);
    if (!c) {
        res.error = "The network library could not start.";
        return res;
    }
    // Options from scratch; the connection and DNS caches stay.
    curl_easy_reset(c);
    Transfer t{&res, req.max_bytes, false, cancel, req.public_only};
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_slist* headers = nullptr;
    for (const std::string& h : req.headers)
        headers = curl_slist_append(headers, h.c_str());
    if (!req.content_type.empty())
        headers = curl_slist_append(headers, ("Content-Type: " + req.content_type).c_str());

    curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, req.follow_redirects ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); // required with threads
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, req.connect_timeout_s);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, req.timeout_s);
    if (req.low_speed_s > 0) {
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, req.low_speed_s);
    }
    curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, on_open_socket);
    curl_easy_setopt(c, CURLOPT_OPENSOCKETDATA, &t);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "DeckReader/" DECKREADER_VERSION);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // whatever curl can decode
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    if (headers)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    if (req.method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
    } else if (req.method != "GET") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    }
    if (req.method != "GET") {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
    }

    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res.status);
    const std::string host = url_host(req.url);
    if (rc != CURLE_OK) {
        res.status = 0;
        res.body.clear();
        switch (rc) {
        case CURLE_ABORTED_BY_CALLBACK:
            res.cancelled = true;
            res.error = "Cancelled.";
            break;
        case CURLE_WRITE_ERROR:
            res.too_large = t.too_large;
            res.error = t.too_large ? "The answer from " + host + " was too large." : "Receiving from " + host + " failed.";
            break;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
            res.error = "The server address must start with https:// or http://.";
            break;
        case CURLE_COULDNT_RESOLVE_HOST:
            res.unreachable = true;
            res.error = "Couldn't find " + host + ". Check the network and the server address.";
            break;
        case CURLE_COULDNT_CONNECT:
            res.unreachable = true;
            res.error = t.refused ? host + " is on this device or the local network: not used."
                                  : "Couldn't connect to " + host + ".";
            break;
        case CURLE_OPERATION_TIMEDOUT:
            res.unreachable = true;
            res.error = host + " didn't answer in time.";
            break;
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CACERT_BADFILE:
            res.unreachable = true;
            res.error = "Secure connection to " + host + " failed" + (errbuf[0] ? std::string(": ") + errbuf : ".");
            break;
        default:
            res.error = host + ": " + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
            break;
        }
    }
    curl_slist_free_all(headers);
    return res;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out += static_cast<char>(ch);
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 15];
        }
    }
    return out;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [k, v] : fields) {
        if (!out.empty())
            out += '&';
        out += url_encode(k) + "=" + url_encode(v);
    }
    return out;
}

} // namespace dr
