#ifndef BYTE_RINGBUFFER_HPP
#define BYTE_RINGBUFFER_HPP

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

class ByteRingbuffer {
public:
    static constexpr size_t BUFFER_SIZE = 262144; // 256KB
    static constexpr size_t BUFFER_MASK = BUFFER_SIZE - 1;

    ByteRingbuffer() : head_(0), tail_(0) {
        // Initialize buffer to silence (zeros)
        std::memset(buffer_, 0, BUFFER_SIZE);
    }

    // Delete copy/move
    ByteRingbuffer(const ByteRingbuffer&) = delete;
    ByteRingbuffer& operator=(const ByteRingbuffer&) = delete;

    // Write data to buffer. Returns bytes actually written (may be less than requested if buffer full)
    size_t write(const uint8_t* src, size_t len) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        
        size_t available = writeAvailable(head, tail);
        if (available == 0) return 0;
        
        size_t to_write = std::min(len, available);
        size_t written = 0;
        
        // Write in two parts if wrapping around
        size_t first_part = std::min(to_write, BUFFER_SIZE - (head & BUFFER_MASK));
        std::memcpy(buffer_ + (head & BUFFER_MASK), src, first_part);
        
        if (first_part < to_write) {
            std::memcpy(buffer_, src + first_part, to_write - first_part);
        }
        
        // Release store to make writes visible to reader
        head_.store(head + to_write, std::memory_order_release);
        
        return to_write;
    }

    // Read data from buffer. Returns bytes actually read (may be less than requested if buffer empty)
    size_t read(uint8_t* dst, size_t len) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        
        size_t available = readAvailable(head, tail);
        if (available == 0) return 0;
        
        size_t to_read = std::min(len, available);
        
        // Read in two parts if wrapping around
        size_t first_part = std::min(to_read, BUFFER_SIZE - (tail & BUFFER_MASK));
        std::memcpy(dst, buffer_ + (tail & BUFFER_MASK), first_part);
        
        if (first_part < to_read) {
            std::memcpy(dst + first_part, buffer_, to_read - first_part);
        }
        
        // Release store to make reads visible to writer
        tail_.store(tail + to_read, std::memory_order_release);
        
        return to_read;
    }

    // Check available bytes for reading
    size_t readAvailable() const {
        size_t head = head_.load(std::memory_order_seq_cst);
        size_t tail = tail_.load(std::memory_order_seq_cst);
        return readAvailable(head, tail);
    }

    // Check available space for writing
    size_t writeAvailable() const {
        size_t head = head_.load(std::memory_order_seq_cst);
        size_t tail = tail_.load(std::memory_order_seq_cst);
        return writeAvailable(head, tail);
    }

    // Clear buffer from consumer side (call before starting new stream)
    void consumerClear() {
        // Acquire head, release tail to synchronize with producer
        size_t head = head_.load(std::memory_order_acquire);
        tail_.store(head, std::memory_order_release);
    }
    
    // Clear from producer side (call when stopping stream)
    void producerClear() {
        // Acquire tail, release head to synchronize with consumer
        size_t tail = tail_.load(std::memory_order_acquire);
        head_.store(tail, std::memory_order_release);
    }

private:
    static size_t readAvailable(size_t head, size_t tail) {
        return head - tail;
    }

    static size_t writeAvailable(size_t head, size_t tail) {
        return BUFFER_SIZE - (head - tail) - 1; // -1 to distinguish full from empty
    }

    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    alignas(64) uint8_t buffer_[BUFFER_SIZE];
};

#endif // BYTE_RINGBUFFER_HPP
