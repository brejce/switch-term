#pragma once
// PTY：fork 一个 shell，master 端非阻塞（对齐 IMPLEMENTATION.md §4.1）
#include <string>
#include <vector>

namespace term {

class Pty {
public:
    ~Pty();

    bool spawn(const std::vector<std::string>& argv, int cols, int rows);
    // 返回读到的字节数；0 = 暂无数据；-1 = 已关闭/出错
    long readMaster(char* buf, size_t n);
    bool writeMaster(const char* buf, size_t n);
    void resize(int cols, int rows);

    int masterFd() const { return fd_; }
    int pid() const { return pid_; }

private:
    int fd_ = -1;
    int pid_ = 0;
};

}  // namespace term
