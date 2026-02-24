#ifndef TUI_HPP
#define TUI_HPP

#include <ncurses.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <array>
#include "fft_spectrum.hpp"

#ifdef ENABLE_MUSICBRAINZ
#include "src/metadata_fetcher.hpp"
#endif

struct Station {
    std::string name;
    std::string url;
};

struct SongHistoryEntry {
    std::string title;
    std::string station_name;
    std::chrono::system_clock::time_point played_at;
};

class RadioTUI {
private:
    // Windows
    WINDOW* header_win_ = nullptr;
    WINDOW* station_win_ = nullptr;
    WINDOW* main_win_ = nullptr;
    WINDOW* controls_win_ = nullptr;
#ifdef METADATA_DEBUG_VIEW
    WINDOW* debug_win_ = nullptr;
#endif
    
    // Dimensions
    int max_y_, max_x_;
    int station_width_ = 30;  // Wider for station names
    
    // State
    std::vector<Station> stations_;
    size_t selected_station_ = 0;
    std::string current_title_;
    std::string current_station_;
    std::vector<SongHistoryEntry> history_;
    std::mutex history_mutex_;
    int buffer_percent_ = 0;
    bool is_playing_ = false;
    int volume_percent_ = 100;  // 0-100
    std::string stream_format_;  // e.g., "MP3 128kbps"
    int stream_kbps_ = 0;        // Current KiB/s
    std::string stream_genre_;   // Genre from ICY/stream metadata
    
    // Track metadata from MusicBrainz
    std::string current_album_;
    std::string current_year_;
    std::string current_genre_;
    bool has_track_metadata_ = false;

#ifdef METADATA_DEBUG_VIEW
    // Debug metadata storage
    std::vector<std::pair<std::string, std::string>> debug_metadata_;
    std::mutex debug_metadata_mutex_;
    
    // Pending debug metadata (thread-safe queue    std::vector from playback thread)
<std::pair<std::string, std::string>> pending_debug_metadata_;
    std::mutex pending_debug_mutex_;
    std::atomic<bool> has_pending_debug_metadata_{false};
    
    static constexpr int DEBUG_WIN_MIN_HEIGHT = 4;
    static constexpr int DEBUG_WIN_MAX_HEIGHT = 15;
    int current_debug_height_ = DEBUG_WIN_MIN_HEIGHT;
    
    // MusicBrainz debug info
    struct MusicBrainzDebugInfo {
        std::string status = "Waiting...";  // "Waiting...", "Querying...", "Received", "Error", "Not found"
        std::string query;                   // What we searched for
        std::string album;
        std::string year;
        std::string genre;
        int score = 0;
        bool has_result = false;
        std::string error_message;
    };
    MusicBrainzDebugInfo mbz_debug_info_;
    std::mutex mbz_debug_mutex_;
    
    // Flag to indicate debug view needs redraw
    std::atomic<bool> debug_needs_redraw_{false};
#endif

    // Callbacks
    std::function<void(const Station&)> on_station_select_;
    std::function<void()> on_stop_;
    std::function<void()> on_quit_;
    std::function<void()> on_volume_up_;
    std::function<void()> on_volume_down_;
    
    // Colors
    int color_header_ = 1;
    int color_selected_ = 2;
    int color_border_ = 3;
    int color_title_ = 4;
    int color_history_ = 5;
    int color_controls_ = 6;
    int color_history_num_ = 7;
    int color_history_station_ = 8;
    int color_history_time_ = 9;
    int color_stopped_ = 10;
    
    // Spectrum visualization
    int color_spectrum_low_ = 11;      // Green (bottom)
    int color_spectrum_mid_ = 12;      // Yellow (middle)
    int color_spectrum_high_ = 13;     // Red (top)
    
    // Spectrum data
    std::array<float, FFTSpectrum::NUM_BARS> spectrum_bars_{};
    bool spectrum_updated_ = false;
    
public:
    RadioTUI();
    ~RadioTUI();
    
    // Initialization
    bool init();
    void cleanup();
    void setup_colors();
    void create_windows();
    void destroy_windows();
    
    // Data setters
    void set_stations(const std::vector<Station>& stations);
    void update_metadata(const std::string& title, const std::string& station);
    void update_buffer(int percent);
    void set_playing(bool playing);
    void set_volume(int percent);
    void update_stream_info(const std::string& format, int kbps);
    void update_stream_genre(const std::string& genre);
    void add_to_history(const std::string& title, const std::string& station);
    void update_track_metadata(const std::string& album, const std::string& year, const std::string& genre);
    void update_spectrum(const std::array<float, FFTSpectrum::NUM_BARS>& bars);
#ifdef METADATA_DEBUG_VIEW
    void update_debug_metadata(const std::string& key, const std::string& value);
    void clear_debug_metadata();
    void queue_debug_metadata(const std::string& key, const std::string& value);
    void process_pending_debug_metadata();
    void update_musicbrainz_debug(const std::string& status,
                                  const std::string& query = "",
                                  const std::string& album = "",
                                  const std::string& year = "",
                                  const std::string& genre = "",
                                  int score = 0,
                                  bool has_result = false,
                                  const std::string& error_message = "");
    void draw_debug();
    bool needsDebugRedraw() { return debug_needs_redraw_.exchange(false); }
#endif
    
    // Drawing
    void draw_all();
    void draw_header();
    void draw_stations();
    void draw_main();
    void draw_controls();
    void draw_spectrum(int y, int max_x);
    void refresh_all();
    
    // Input handling
    int get_input();  // Non-blocking
    void handle_input(int ch);
    
    // Navigation
    void next_station();
    void prev_station();
    void select_station();
    
    // Callbacks
    void set_on_station_select(std::function<void(const Station&)> cb);
    void set_on_stop(std::function<void()> cb);
    void set_on_quit(std::function<void()> cb);
    void set_on_volume_up(std::function<void()> cb);
    void set_on_volume_down(std::function<void()> cb);
    
    // Helpers
    void show_message(const std::string& msg);
    std::string format_time_ago(const std::chrono::system_clock::time_point& tp);
    std::string format_time_clock(const std::chrono::system_clock::time_point& tp);
};

#endif // TUI_HPP
