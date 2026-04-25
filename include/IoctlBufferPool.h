#pragma once
#include <cstddef>
#include <cstdlib>
#include <cstring>

class IoctlBufferPool {
public:
    IoctlBufferPool() : buf_(nullptr), capacity_(0) {}
    ~IoctlBufferPool() { std::free(buf_); }

    char* getBuffer(size_t size) {
        if (size > capacity_) {
            char* newBuf = static_cast<char*>(std::realloc(buf_, size));
            if (!newBuf) return nullptr;
            buf_ = newBuf;
            capacity_ = size;
        }
        std::memset(buf_, 0, size);
        return buf_;
    }

private:
    char* buf_;
    size_t capacity_;
};
