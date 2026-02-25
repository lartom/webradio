#include "fft_spectrum.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

// Separate attack (rising) and decay (falling) factors like cava
// Attack: how fast bars rise (lower = faster rise)
// Decay: how fast bars fall (higher = slower fall)
static constexpr float ATTACK_FACTOR = 0.60f;   // Quick rise
static constexpr float DECAY_FACTOR = 0.85f;    // Slow fall

// Minimum and maximum frequency for visualization
static constexpr float MIN_FREQ = 30.0f;   // Hz - lower for better bass response
static constexpr float MAX_FREQ = 16000.0f; // Hz

void FFTSpectrum::SampleBuffer::push_mono(const int16_t* stereo, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        // Convert stereo s16 to mono float (-1.0 to 1.0)
        float left = stereo[i * 2] / 32768.0f;
        float right = stereo[i * 2 + 1] / 32768.0f;
        float mono = (left + right) * 0.5f;
        
        size_t pos = write_pos.load(std::memory_order_relaxed);
        samples[pos] = mono;
        
        size_t next_pos = (pos + 1) % SIZE;
        write_pos.store(next_pos, std::memory_order_release);
        
        // If buffer is full, advance read position (overwrite oldest)
        if (next_pos == read_pos.load(std::memory_order_acquire)) {
            read_pos.store((next_pos + 1) % SIZE, std::memory_order_relaxed);
        }
    }
}

size_t FFTSpectrum::SampleBuffer::available() const {
    size_t write = write_pos.load(std::memory_order_acquire);
    size_t read = read_pos.load(std::memory_order_acquire);
    
    if (write >= read) {
        return write - read;
    } else {
        return SIZE - read + write;
    }
}

bool FFTSpectrum::SampleBuffer::read_block(float* out, size_t count) {
    if (available() < count) {
        return false;
    }
    
    size_t read = read_pos.load(std::memory_order_relaxed);
    
    for (size_t i = 0; i < count; ++i) {
        out[i] = samples[read];
        read = (read + 1) % SIZE;
    }
    
    read_pos.store(read, std::memory_order_release);
    return true;
}

FFTSpectrum::FFTSpectrum() 
    : fft_input_(FFT_SIZE)
    , fft_real_(FFT_SIZE / 2 + 1)
    , fft_imag_(FFT_SIZE / 2 + 1)
    , smoothed_magnitudes_(NUM_BARS, 0.0f)
    , bar_peaks_(NUM_BARS, MIN_PEAK)
    , window_(FFT_SIZE)
    , bar_ranges_(NUM_BARS)
{
    init_window();
    init_bar_ranges();
    spectrum_data_.updated.store(false);
}

FFTSpectrum::~FFTSpectrum() = default;

void FFTSpectrum::init_window() {
    // Hann window for better frequency resolution
    for (int i = 0; i < FFT_SIZE; ++i) {
        window_[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (FFT_SIZE - 1)));
    }
}

void FFTSpectrum::init_bar_ranges() {
    // Cava-style explicit frequency boundaries for 16 bars
    // Based on typical cava configuration with proper octave spacing

    const int MAX_BIN = FFT_SIZE / 2;
    const float bin_size = static_cast<float>(SAMPLE_RATE) / FFT_SIZE;

    // Explicit frequency boundaries (low to high) - cava-style
    // Extended to cover full spectrum with better high-freq distribution
    float boundaries[NUM_BARS + 1] = {
        30.0f,    // Bar 0 start
        60.0f,    // Bar 1 start
        90.0f,    // Bar 2 start
        120.0f,   // Bar 3 start
        160.0f,   // Bar 4 start
        200.0f,   // Bar 5 start
        250.0f,   // Bar 6 start
        315.0f,   // Bar 7 start
        400.0f,   // Bar 8 start
        500.0f,   // Bar 9 start
        630.0f,   // Bar 10 start
        800.0f,   // Bar 11 start
        1000.0f,  // Bar 12 start
        1600.0f,  // Bar 13 start
        2500.0f,  // Bar 14 start
        4000.0f,  // Bar 15 start
        10000.0f  // End
    };

    for (int bar = 0; bar < NUM_BARS; ++bar) {
        float f_low = boundaries[bar];
        float f_high = boundaries[bar + 1];

        // Convert to bins
        int bin_start = static_cast<int>(f_low / bin_size);
        int bin_end = static_cast<int>(f_high / bin_size);

        // Ensure valid ranges
        bin_start = std::max(1, bin_start);  // Skip DC
        if (bin_end <= bin_start) {
            bin_end = bin_start + 1;
        }
        // Cap bins per bar for consistent visual weight
        if (bin_end - bin_start > 60) {
            bin_end = bin_start + 60;
        }
        bin_end = std::min(MAX_BIN, bin_end);

        bar_ranges_[bar] = {bin_start, bin_end};
    }
}

void FFTSpectrum::dft_real(const float* input, float* real_out, float* imag_out, int n) {
    // Simple DFT for real input (optimized version)
    int n2 = n / 2 + 1;
    float scale = 1.0f / n;  // Normalize by FFT size
    
    for (int k = 0; k < n2; ++k) {
        float real = 0.0f;
        float imag = 0.0f;
        
        // Use symmetry for real input
        for (int n_idx = 0; n_idx < n; ++n_idx) {
            float angle = -2.0f * M_PI * k * n_idx / n;
            real += input[n_idx] * std::cos(angle);
            imag += input[n_idx] * std::sin(angle);
        }
        
        // Normalize to prevent overflow and get consistent magnitudes
        real_out[k] = real * scale;
        imag_out[k] = imag * scale;
    }
}

void FFTSpectrum::compute_fft() {
    // Apply window function
    for (int i = 0; i < FFT_SIZE; ++i) {
        fft_input_[i] *= window_[i];
    }
    
    // Compute DFT
    dft_real(fft_input_.data(), fft_real_.data(), fft_imag_.data(), FFT_SIZE);
}

void FFTSpectrum::update_spectrum() {
    // Compute magnitude for each frequency bin
    std::vector<float> magnitudes(FFT_SIZE / 2 + 1);
    for (size_t i = 0; i < magnitudes.size(); ++i) {
        magnitudes[i] = std::sqrt(fft_real_[i] * fft_real_[i] + fft_imag_[i] * fft_imag_[i]);
    }

    // Average magnitudes for each bar
    std::array<float, NUM_BARS> new_bars{};

    for (int bar = 0; bar < NUM_BARS; ++bar) {
        int start_bin = bar_ranges_[bar].first;
        int end_bin = bar_ranges_[bar].second;

        if (start_bin >= end_bin) {
            new_bars[bar] = 0.0f;
            continue;
        }

        // Average magnitude in this frequency range
        float sum = 0.0f;
        for (int bin = start_bin; bin < end_bin; ++bin) {
            sum += magnitudes[bin];
        }
        float avg = sum / (end_bin - start_bin);

        // Apply power law scaling for better visual dynamics
        float raw_magnitude = std::sqrt(avg) * 2.0f;

        // AUTOGAIN: Track peak and normalize
        // Update running peak with slow decay
        bar_peaks_[bar] *= AUTOGAIN_DECAY;
        if (raw_magnitude > bar_peaks_[bar]) {
            bar_peaks_[bar] = raw_magnitude;
        }
        // Ensure minimum peak to prevent division issues
        if (bar_peaks_[bar] < MIN_PEAK) {
            bar_peaks_[bar] = MIN_PEAK;
        }

        // Normalize against peak to make all bars equally visible
        float normalized = raw_magnitude / bar_peaks_[bar];

        // Apply sensitivity curve - boost mid-range for better visibility
        normalized = std::pow(normalized, 0.7f);  // Gamma < 1 boosts quieter signals

        normalized = std::max(0.0f, std::min(1.0f, normalized));

        normalized = std::max(0.0f, std::min(1.0f, normalized));

        // Apply separate attack/decay with gravity effect like cava
        float current = smoothed_magnitudes_[bar];
        float diff = normalized - current;

        if (diff > 0) {
            // Rising - attack quickly
            smoothed_magnitudes_[bar] = current + diff * (1.0f - ATTACK_FACTOR);
        } else {
            // Falling - decay with gravity (fall faster when higher)
            float gravity = 0.01f + (current * 0.04f);  // Higher bars fall faster
            float fall_amount = std::max(std::abs(diff) * (1.0f - DECAY_FACTOR), gravity);
            smoothed_magnitudes_[bar] = std::max(0.0f, current - fall_amount);
        }

        new_bars[bar] = smoothed_magnitudes_[bar];
    }
    
    // Update spectrum data (atomic swap)
    for (int i = 0; i < NUM_BARS; ++i) {
        spectrum_data_.bars[i] = new_bars[i];
    }
    spectrum_data_.updated.store(true, std::memory_order_release);
}

void FFTSpectrum::push_samples(const int16_t* stereo_samples, size_t frame_count)
{
	std::lock_guard<std::mutex> lock(sample_buffer_mutex);
    // Push samples to ring buffer
    sample_buffer_.push_mono(stereo_samples, frame_count);
} 

void FFTSpectrum::process_samples()
{
   
    // Process FFT when we have enough samples
    static auto last_update = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update).count();
    
    if (elapsed >= UPDATE_INTERVAL_MS && sample_buffer_.available() >= FFT_SIZE) {

		sample_buffer_mutex.lock();
		sample_buffer_.read_block(fft_input_.data(), FFT_SIZE);
		sample_buffer_mutex.unlock();

		// Read block of samples
        compute_fft();
        update_spectrum();
        last_update = now;
    }
}

void FFTSpectrum::get_spectrum(std::array<float, NUM_BARS>& out_bars, bool& out_updated) {
    // Copy current data
    for (int i = 0; i < NUM_BARS; ++i) {
        out_bars[i] = spectrum_data_.bars[i];
    }
    out_updated = spectrum_data_.updated.exchange(false, std::memory_order_acq_rel);
}
