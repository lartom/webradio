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
    WINDOW* header_win_ = nullptr;
    WINDOW* station_win_ = nullptr;
    WINDOW* main_win_ = nullptr;
    WINDOW* controls_win_ = nullptr;
    
    int max_y_, max_x_;
    int station_width_ = 30;
    
    std::vector<Station> stations_;
    size_t selected_station_ = 0;
    std::string current_title_;
    std::string current_station_;
    std::vector<SongHistoryEntry> history_;
    std::mutex history_mutex_;
    int buffer_percent_ = 0;
    bool is_playing_ = false;
    int volume_percent_ = 100;
    std::string stream_format_;
    int stream_kbps_ = 0;
    std::string stream_genre_;
    
    std::string current_album_;
    std::string current_year_;
    std::string current_genre_;
    bool has_track_metadata_ = false;

    std::function<void(const Station&)> on_station_select_;
    std::function<void()> on_stop_;
    std::function<void()> on_quit_;
    std::function<void()> on_volume_up_;
    std::function<void()> on_volume_down_;
    
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
    
    int color_spectrum_low_ = 11;
    int color_spectrum_mid_ = 12;
    int color_spectrum_high_ = 13;
    
    std::array<float, FFTSpectrum::NUM_BARS> spectrum_bars_{};
    bool spectrum_updated_ = false;
    
public:
    RadioTUI();
    ~RadioTUI();
    
    bool init();
    void cleanup();
    void setup_colors();
    void create_windows();
    void destroy_windows();
    
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
    
    void draw_all();
    void draw_header();
    void draw_stations();
    void draw_main();
    void draw_controls();
    void draw_spectrum(int y, int max_x);
    void refresh_all();
    
    int get_input();
    void handle_input(int ch);
    
    void next_station();
    void prev_station();
    void select_station();
    
    void set_on_station_select(std::function<void(const Station&)> cb);
    void set_on_stop(std::function<void()> cb);
    void set_on_quit(std::function<void()> cb);
    void set_on_volume_up(std::function<void()> cb);
    void set_on_volume_down(std::function<void()> cb);
    
    void show_message(const std::string& msg);
    std::string format_time_ago(const std::chrono::system_clock::time_point& tp);
    std::string format_time_clock(const std::chrono::system_clock::time_point& tp);
};

#endif // TUI_HPP
