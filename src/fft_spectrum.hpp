#ifndef FFT_SPECTRUM_HPP
#define FFT_SPECTRUM_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>	

// Simple FFT implementation using DFT (no external dependencies)
// Optimized for real-time audio visualization

class FFTSpectrum {
public:
    static constexpr int NUM_BARS = 16;
    static constexpr int FFT_SIZE = 2048;
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr int UPDATE_INTERVAL_MS = 33;  // ~30 FPS
    
    struct SpectrumData {
        std::array<float, NUM_BARS> bars{};
        std::atomic<bool> updated{false};
    };

    FFTSpectrum();
    ~FFTSpectrum();
    
    // Called from audio callback thread - lock-free
 	void push_samples(const int16_t* stereo_samples, size_t frame_count);
 
	void process_samples();
    
    // Called from main thread to get latest spectrum
    void get_spectrum(std::array<float, NUM_BARS>& out_bars, bool& out_updated);
    
    // Check if spectrum has been updated since last get
    bool has_new_data() const { return spectrum_data_.updated.load(); }

private:
    // Ring buffer for audio samples (lock-free, single producer)
    struct SampleBuffer {
        static constexpr size_t SIZE = FFT_SIZE * 4;  // 4x FFT size for overlap
        std::array<float, SIZE> samples{};
        std::atomic<size_t> write_pos{0};
        std::atomic<size_t> read_pos{0};
        
        void push_mono(const int16_t* stereo, size_t frames);
        bool read_block(float* out, size_t count);
        size_t available() const;
    };
    
	std::mutex sample_buffer_mutex;
    SampleBuffer sample_buffer_;
    SpectrumData spectrum_data_;
    
    // FFT working buffers
    std::vector<float> fft_input_;
    std::vector<float> fft_real_;
    std::vector<float> fft_imag_;
    std::vector<float> smoothed_magnitudes_;
    
    // Autogain: track recent peaks per bar for normalization
    std::vector<float> bar_peaks_;
    static constexpr float AUTOGAIN_DECAY = 0.995f;  // Slow decay of peak
    static constexpr float MIN_PEAK = 0.001f;        // Prevent division by zero
    
    // Window function (Hann)
    std::vector<float> window_;
    
    // Frequency bin mapping (logarithmic)
    std::vector<std::pair<int, int>> bar_ranges_;
    
    void compute_fft();
    void update_spectrum();
    void init_window();
    void init_bar_ranges();
    
    // DFT computation for real input
    void dft_real(const float* input, float* real_out, float* imag_out, int n);
};

#endif // FFT_SPECTRUM_HPP
