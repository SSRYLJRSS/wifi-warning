#include "ui/http_server.h"

#include "core/util.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ww {

static constexpr int kMaxActiveClients = 4;
static constexpr size_t kMaxHeaderBytes = 16 * 1024;
static constexpr size_t kMaxBodyBytes = 2 * 1024 * 1024;

static std::string statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

static std::string contentTypeFor(const fs::path& path) {
    auto ext = toLowerAscii(wideToUtf8(path.extension().wstring()));
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

static std::map<std::string, std::string> parseQuery(const std::string& query) {
    std::map<std::string, std::string> params;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string part = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!part.empty()) {
            size_t eq = part.find('=');
            std::string key = urlDecode(part.substr(0, eq));
            std::string value = eq == std::string::npos ? "" : urlDecode(part.substr(eq + 1));
            params[key] = value;
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return params;
}

static HttpRequest parseRequest(const std::string& raw) {
    HttpRequest request;
    std::istringstream stream(raw);
    std::string first;
    std::getline(stream, first);
    if (!first.empty() && first.back() == '\r') first.pop_back();

    std::istringstream firstLine(first);
    std::string target;
    firstLine >> request.method >> target;
    size_t q = target.find('?');
    request.path = q == std::string::npos ? target : target.substr(0, q);
    request.query = q == std::string::npos ? "" : target.substr(q + 1);
    request.query_params = parseQuery(request.query);

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = toLowerAscii(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        request.headers[key] = value;
    }

    auto bodyPos = raw.find("\r\n\r\n");
    if (bodyPos != std::string::npos) request.body = raw.substr(bodyPos + 4);
    return request;
}

static std::string responseBytes(const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << statusText(response.status) << "\r\n";
    out << "Content-Type: " << response.content_type << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Access-Control-Allow-Origin: http://localhost\r\n";
    out << "Cache-Control: no-store\r\n";
    for (const auto& [key, value] : response.headers) out << key << ": " << value << "\r\n";
    out << "\r\n";
    out << response.body;
    return out.str();
}

static bool sendAll(SOCKET client, const std::string& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        int chunk = send(client, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
        if (chunk <= 0) return false;
        sent += static_cast<size_t>(chunk);
    }
    return true;
}

static std::string readBinary(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static fs::path frontendRoot() {
    fs::path exe = executableDir();
    if (fs::exists(exe / L"frontend")) return exe / L"frontend";
    fs::path cwd = currentWorkingDir();
    if (fs::exists(cwd / L"frontend")) return cwd / L"frontend";
    return exe / L"frontend";
}

static bool pathWithinRoot(const fs::path& root, const fs::path& file) {
    std::wstring rootText = root.wstring();
    std::wstring fileText = file.wstring();
    while (rootText.size() > 3 && (rootText.back() == L'\\' || rootText.back() == L'/')) rootText.pop_back();
    if (fileText.size() < rootText.size()) return false;
    if (CompareStringOrdinal(fileText.c_str(), static_cast<int>(rootText.size()), rootText.c_str(), static_cast<int>(rootText.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    if (fileText.size() == rootText.size()) return true;
    wchar_t next = fileText[rootText.size()];
    return next == L'\\' || next == L'/';
}

HttpServer::HttpServer(int port, ConfigManager& config, Logger& logger)
    : port_(port), config_(config), logger_(logger), api_(std::make_unique<ApiHandlers>(config_, logger_)) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (running_) return true;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    if (!running_) run();
    if (!running_) {
        WSACleanup();
        return false;
    }
    worker_ = std::thread([this] { acceptLoop(); });
    return running_;
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;
    if (listen_socket_) {
        closesocket(static_cast<SOCKET>(listen_socket_));
        listen_socket_ = 0;
    }
    if (worker_.joinable()) worker_.join();
    std::unique_lock lock(clients_mutex_);
    clients_done_.wait(lock, [this] { return active_clients_.load() == 0; });
    WSACleanup();
}

int HttpServer::port() const {
    return port_;
}

void HttpServer::run() {
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        running_ = false;
        return;
    }
    listen_socket_ = server;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port_));
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(server, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server);
        running_ = false;
        return;
    }

    running_ = true;
}

void HttpServer::acceptLoop() {
    SOCKET server = static_cast<SOCKET>(listen_socket_);
    while (running_) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (running_) Sleep(20);
            continue;
        }
        DWORD timeoutMs = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        if (active_clients_.load() >= kMaxActiveClients) {
            HttpResponse busy;
            busy.status = 409;
            busy.body = "{\"ok\":false,\"error\":\"too many active requests\"}\n";
            std::string bytes = responseBytes(busy);
            sendAll(client, bytes);
            shutdown(client, SD_BOTH);
            closesocket(client);
            continue;
        }
        active_clients_.fetch_add(1);
        try {
            std::thread(&HttpServer::handleClient, this, static_cast<uintptr_t>(client)).detach();
        } catch (...) {
            if (active_clients_.fetch_sub(1) == 1) clients_done_.notify_all();
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }
}

void HttpServer::handleClient(uintptr_t clientSocket) {
    SOCKET client = static_cast<SOCKET>(clientSocket);
    std::string raw;
    char buffer[4096];
    HttpResponse response;
    bool hasResponse = false;
    while (raw.find("\r\n\r\n") == std::string::npos && raw.size() <= kMaxHeaderBytes) {
        int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        raw.append(buffer, buffer + received);
    }

    auto headersEnd = raw.find("\r\n\r\n");
    if (headersEnd == std::string::npos) {
        response = errorResponse("请求头不完整", 400);
        hasResponse = true;
    } else {
        auto request = parseRequest(raw);
        size_t contentLength = 0;
        if (auto it = request.headers.find("content-length"); it != request.headers.end()) {
            if (auto parsed = parseInt(it->second); parsed && *parsed >= 0) contentLength = static_cast<size_t>(*parsed);
        }
        if (contentLength > kMaxBodyBytes) {
            response = errorResponse("请求体过大", 400);
            hasResponse = true;
        } else {
            while (raw.size() < headersEnd + 4 + contentLength) {
                int received = recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                raw.append(buffer, buffer + received);
            }
            if (raw.size() < headersEnd + 4 + contentLength) {
                response = errorResponse("请求体不完整", 400);
                hasResponse = true;
            }
        }
    }

    if (!raw.empty()) {
        if (!hasResponse) response = route(parseRequest(raw));
        std::string bytes = responseBytes(response);
        sendAll(client, bytes);
    }
    shutdown(client, SD_BOTH);
    closesocket(client);
    if (active_clients_.fetch_sub(1) == 1) clients_done_.notify_all();
}

HttpResponse HttpServer::route(const HttpRequest& request) {
    HttpResponse response;
    if (api_->handle(request, response)) return response;

    fs::path root = frontendRoot();
    fs::path file;
    if (request.path == "/" || request.path == "/index.html") file = root / L"index.html";
    else if (request.path == "/settings") file = root / L"settings.html";
    else if (request.path == "/warning") file = root / L"warning.html";
    else if (request.path == "/wifi-picker") file = root / L"wifi_picker.html";
    else if (request.path == "/favicon.ico") file = root / L"assets\\icons\\favicon.ico";
    else {
        std::string clean = request.path;
        while (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());
        file = root / utf8ToWide(clean);
    }

    std::error_code ec;
    fs::path canonicalRoot = fs::weakly_canonical(root, ec);
    fs::path canonicalFile = fs::weakly_canonical(file, ec);
    if (ec || !pathWithinRoot(canonicalRoot, canonicalFile) || !fs::exists(file, ec)) {
        return errorResponse("未找到页面", 404);
    }

    response.content_type = contentTypeFor(file);
    response.body = readBinary(file);
    return response;
}

}
