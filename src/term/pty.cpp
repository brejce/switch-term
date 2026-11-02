#include "term/pty.h"

#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace term {

Pty::~Pty() {
    if (fd_ >= 0) ::close(fd_);
    if (pid_ > 0) {
        ::kill(pid_, SIGHUP);
        ::waitpid(pid_, nullptr, WNOHANG);
    }
}

bool Pty::spawn(const std::vector<std::string>& argv, int cols, int rows) {
    if (argv.empty()) return false;
    struct winsize ws{};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    pid_ = ::forkpty(&fd_, nullptr, nullptr, &ws);
    if (pid_ < 0) {
        std::fprintf(stderr, "forkpty: %s\n", std::strerror(errno));
        return false;
    }
    if (pid_ == 0) {
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 0);
        std::vector<char*> av;
        for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        ::execvp(av[0], av.data());
        std::perror("execvp");
        ::_exit(127);
    }

    int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    return true;
}

long Pty::readMaster(char* buf, size_t n) {
    for (;;) {
        ssize_t r = ::read(fd_, buf, n);
        if (r > 0) return long(r);
        if (r == 0) return -1;  // EOF
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

bool Pty::writeMaster(const char* buf, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd_, buf, n);
        if (w > 0) {
            buf += w;
            n -= size_t(w);
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void Pty::resize(int cols, int rows) {
    struct winsize ws{};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ::ioctl(fd_, TIOCSWINSZ, &ws);
}

}  // namespace term
