#include "config.h"
#if defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
#include "rpc_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#ifdef WIN32
#include <windows.h>
#include <sddl.h>
#pragma comment(lib, "Advapi32.lib")
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace dosbox_agent {
namespace {

class NamedPipeTransport final : public IRpcTransport {
public:
    NamedPipeTransport() : impl(new Impl()) {}
    ~NamedPipeTransport() override { Stop(); }

    const char* Name() const override { return "named_pipe"; }

    bool ValidateConfiguration(const AgentConfig& config,
                               std::string* error) const override
    {
        if (config.endpoint.compare(0, 9, "\\\\.\\pipe\\") != 0 || config.endpoint.size() == 9) {
            if (error != NULL)
                *error = "Named pipe endpoint must use \\\\.\\pipe\\<name>";
            return false;
        }
        return true;
    }

    bool Start(const AgentConfig& config,
               RequestHandler handler,
               std::string* error) override
    {
        if (!ValidateConfiguration(config, error) || !handler)
            return false;

#ifndef WIN32
        if (error != NULL)
            *error = "Windows named pipes are unavailable on this platform";
        return false;
#else
        std::unique_lock<std::mutex> lock(impl->mutex);
        if (impl->listener.joinable()) {
            if (error != NULL)
                *error = "Named pipe transport is already running";
            return false;
        }

        impl->config = config;
        impl->handler = handler;
        impl->stopping = false;
        impl->ready = false;
        impl->start_succeeded = false;
        impl->start_error.clear();
        impl->listener = std::thread(&NamedPipeTransport::Run, this);
        impl->ready_condition.wait(lock, [this]() { return impl->ready; });
        if (impl->start_succeeded)
            return true;

        const std::string start_error = impl->start_error;
        lock.unlock();
        impl->listener.join();
        if (error != NULL)
            *error = start_error;
        return false;
#endif
    }

    void Stop() override
    {
#ifdef WIN32
        std::thread listener;
        std::vector<std::thread> client_workers;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->listener.joinable())
                return;
            impl->stopping = true;
            for (std::vector<HANDLE>::const_iterator it = impl->active_pipes.begin();
                 it != impl->active_pipes.end(); ++it)
                CancelIoEx(*it, NULL);
            listener.swap(impl->listener);
        }
        listener.join();
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            client_workers.swap(impl->client_workers);
        }
        for (std::vector<std::thread>::iterator it = client_workers.begin();
             it != client_workers.end(); ++it)
            if (it->joinable())
                it->join();
#endif
    }

private:
    class Impl {
    public:
        std::mutex mutex;
        std::condition_variable ready_condition;
        AgentConfig config;
        RequestHandler handler;
        std::thread listener;
        bool stopping = false;
        bool ready = false;
        bool start_succeeded = false;
        std::string start_error;
#ifdef WIN32
        HANDLE active_pipe = INVALID_HANDLE_VALUE;
        std::vector<HANDLE> active_pipes;
        std::vector<std::thread> client_workers;
#endif
    };

    std::unique_ptr<Impl> impl;

#ifdef WIN32
    static bool WriteAll(const HANDLE pipe, const std::string& message)
    {
        std::size_t offset = 0;
        while (offset < message.size()) {
            const DWORD remaining = static_cast<DWORD>((std::min)(message.size() - offset,
                                                                    static_cast<std::size_t>(0xffffffffu)));
            DWORD written = 0;
            if (!WriteFile(pipe, message.data() + offset, remaining, &written, NULL) || written == 0)
                return false;
            offset += written;
        }
        return true;
    }

    HANDLE CreatePipe(std::string* error)
    {
        PSECURITY_DESCRIPTOR descriptor = NULL;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA("D:P(A;;GA;;;OW)",
                                                                    SDDL_REVISION_1,
                                                                    &descriptor,
                                                                    NULL)) {
            *error = "Unable to create named pipe security descriptor";
            return INVALID_HANDLE_VALUE;
        }

        SECURITY_ATTRIBUTES attributes;
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        const DWORD buffer_size = static_cast<DWORD>((std::min)(impl->config.max_message_bytes,
                                                                 static_cast<std::size_t>(0xffffffffu)));
        HANDLE pipe = CreateNamedPipeA(impl->config.endpoint.c_str(),
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       buffer_size,
                                       buffer_size,
                                       0,
                                       &attributes);
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE)
            *error = "Unable to create named pipe endpoint";
        return pipe;
    }

    void RemoveActivePipe(const HANDLE pipe)
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        for (std::vector<HANDLE>::iterator it = impl->active_pipes.begin();
             it != impl->active_pipes.end(); ++it) {
            if (*it == pipe) {
                impl->active_pipes.erase(it);
                break;
            }
        }
    }

    bool ServeClient(const HANDLE pipe)
    {
        std::string pending;
        char buffer[4096];
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &read, NULL) || read == 0)
                return false;
            pending.append(buffer, read);

            for (;;) {
                const std::string::size_type line_end = pending.find('\n');
                if (line_end == std::string::npos) {
                    if (pending.size() > impl->config.max_message_bytes) {
                        const std::string response = impl->handler(std::string(impl->config.max_message_bytes + 1, 'x')) + "\n";
                        WriteAll(pipe, response);
                        return false;
                    }
                    break;
                }

                std::string request = pending.substr(0, line_end);
                pending.erase(0, line_end + 1);
                if (!request.empty() && request[request.size() - 1] == '\r')
                    request.erase(request.size() - 1);
                if (request.size() > impl->config.max_message_bytes) {
                    const std::string response = impl->handler(std::string(impl->config.max_message_bytes + 1, 'x')) + "\n";
                    WriteAll(pipe, response);
                    return false;
                }
                if (!WriteAll(pipe, impl->handler(request) + "\n"))
                    return false;
            }

            std::lock_guard<std::mutex> lock(impl->mutex);
            if (impl->stopping)
                return false;
        }
    }

    void ServeClientThread(const HANDLE pipe)
    {
        ServeClient(pipe);
        CloseHandle(pipe);
        RemoveActivePipe(pipe);
    }

    void Run()
    {
        bool first_pipe = true;
        for (;;) {
            std::string error;
            const HANDLE pipe = CreatePipe(&error);
            if (pipe == INVALID_HANDLE_VALUE) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                if (first_pipe) {
                    impl->start_error = error;
                    impl->ready = true;
                    impl->ready_condition.notify_all();
                }
                return;
            }

            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->active_pipe = pipe;
                impl->active_pipes.push_back(pipe);
                if (first_pipe) {
                    impl->start_succeeded = true;
                    impl->ready = true;
                    impl->ready_condition.notify_all();
                    first_pipe = false;
                }
                if (impl->stopping) {
                    CloseHandle(pipe);
                    impl->active_pipe = INVALID_HANDLE_VALUE;
                    impl->active_pipes.pop_back();
                    return;
                }
            }

            const BOOL connected = ConnectNamedPipe(pipe, NULL);
            const DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
            if (connected || connect_error == ERROR_PIPE_CONNECTED) {
                bool stopping = false;
                {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    stopping = impl->stopping;
                    if (!stopping)
                        impl->client_workers.push_back(std::thread(&NamedPipeTransport::ServeClientThread, this, pipe));
                }
                if (stopping) {
                    CloseHandle(pipe);
                    RemoveActivePipe(pipe);
                    return;
                }
            } else {
                CloseHandle(pipe);
                RemoveActivePipe(pipe);
            }

            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->active_pipe = INVALID_HANDLE_VALUE;
            if (impl->stopping)
                return;
        }
    }
#endif
};

class UnixSocketTransport final : public IRpcTransport {
public:
    ~UnixSocketTransport() override { Stop(); }

    const char* Name() const override { return "unix_socket"; }

    bool ValidateConfiguration(const AgentConfig& config,
                               std::string* error) const override
    {
        if (config.endpoint.empty() || config.endpoint[0] != '/') {
            if (error != NULL)
                *error = "Unix socket endpoint must be an absolute path";
            return false;
        }
#ifndef WIN32
        if (config.endpoint.size() >= sizeof(sockaddr_un::sun_path)) {
            if (error) *error = "Unix socket endpoint exceeds sun_path capacity";
            return false;
        }
        const std::string parent = config.endpoint.substr(0, config.endpoint.rfind('/'));
        struct stat directory = {};
        if (stat(parent.c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode) ||
            directory.st_uid != geteuid() || (directory.st_mode & 0077) != 0) {
            if (error) *error = "Unix socket requires an owned private parent directory (0700)";
            return false;
        }
#endif
        return true;
    }

    bool Start(const AgentConfig& config,
               RequestHandler handler,
               std::string* error) override
    {
#ifdef WIN32
        if (error) *error = "Unix socket transport is unavailable on Windows";
        return false;
#else
        if (!ValidateConfiguration(config, error) || !handler)
            return false;
        if (listener >= 0) {
            if (error) *error = "Unix socket transport is already running";
            return false;
        }
        listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (listener < 0) {
            if (error) *error = std::string("socket: ") + strerror(errno);
            return false;
        }
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, config.endpoint.c_str(), config.endpoint.size() + 1);
        // Never replace an existing endpoint or file.
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            if (error) *error = std::string("bind: ") + strerror(errno);
            close(listener);
            listener = -1;
            return false;
        }
        endpoint = config.endpoint;
        if (lstat(endpoint.c_str(), &owned_endpoint) != 0 ||
            chmod(endpoint.c_str(), 0600) != 0 || listen(listener, 8) != 0) {
            if (error) *error = std::string("Unix socket setup: ") + strerror(errno);
            Stop();
            return false;
        }
        settings = config;
        callback = handler;
        stopping = false;
        worker = std::thread(&UnixSocketTransport::Run, this);
        return true;
#endif
    }

    void Stop() override
    {
#ifndef WIN32
        stopping = true;
        if (worker.joinable()) worker.join();
        if (listener >= 0) close(listener);
        listener = -1;
        struct stat current = {};
        if (!endpoint.empty() && lstat(endpoint.c_str(), &current) == 0 &&
            current.st_dev == owned_endpoint.st_dev && current.st_ino == owned_endpoint.st_ino)
            unlink(endpoint.c_str());
        endpoint.clear();
#endif
    }

private:
#ifndef WIN32
    int listener = -1;
    std::atomic<bool> stopping{false};
    std::thread worker;
    std::string endpoint;
    struct stat owned_endpoint = {};
    AgentConfig settings;
    RequestHandler callback;
    typedef std::chrono::steady_clock Clock;

    bool Ready(int fd, short events, Clock::time_point deadline)
    {
        while (!stopping && Clock::now() < deadline) {
            pollfd descriptor = {fd, events, 0};
            const int result = poll(&descriptor, 1, 100);
            if (result < 0 && errno != EINTR) return false;
            if (result > 0) return (descriptor.revents & events) != 0;
        }
        return false;
    }

    bool Send(int fd, const std::string& response)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(settings.request_timeout_ms);
        std::size_t offset = 0;
        while (offset < response.size()) {
            if (!Ready(fd, POLLOUT, deadline)) return false;
            const ssize_t count = send(fd, response.data() + offset,
                                      response.size() - offset, MSG_NOSIGNAL);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) return false;
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    void Serve(int fd)
    {
        std::string pending;
        char buffer[4096];
        auto deadline = Clock::now() + std::chrono::milliseconds(settings.request_timeout_ms);
        while (Ready(fd, POLLIN, deadline)) {
            const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) return;
            pending.append(buffer, static_cast<std::size_t>(count));
            for (;;) {
                const auto newline = pending.find('\n');
                if ((newline == std::string::npos && pending.size() > settings.max_message_bytes) ||
                    (newline != std::string::npos && newline > settings.max_message_bytes)) {
                    Send(fd, callback(std::string(settings.max_message_bytes + 1, 'x')) + "\n");
                    return;
                }
                if (newline == std::string::npos) break;
                std::string request = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!request.empty() && request.back() == '\r') request.pop_back();
                if (!Send(fd, callback(request) + "\n")) return;
                deadline = Clock::now() + std::chrono::milliseconds(settings.request_timeout_ms);
            }
        }
    }

    void Run()
    {
        while (!stopping) {
            if (!Ready(listener, POLLIN, Clock::now() + std::chrono::milliseconds(100))) continue;
            const int fd = accept(listener, NULL, NULL);
            if (fd < 0) continue;
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
                fcntl(fd, F_SETFL, O_NONBLOCK) == 0) {
                try { Serve(fd); } catch (...) { /* Close a failed client; keep the endpoint alive. */ }
            }
            close(fd);
        }
    }
#endif
};

} // namespace

std::unique_ptr<IRpcTransport> AGENT_CreateLocalTransport(const AgentTransport transport)
{
    switch (transport) {
    case AgentTransport::NamedPipe:
        return std::unique_ptr<IRpcTransport>(new NamedPipeTransport());
    case AgentTransport::UnixSocket:
        return std::unique_ptr<IRpcTransport>(new UnixSocketTransport());
    }
    return std::unique_ptr<IRpcTransport>();
}

} // namespace dosbox_agent
#endif // defined(C_DEBUG) && defined(C_DOSBOX_AGENT)
