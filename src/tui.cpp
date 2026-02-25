#include "tui.hpp"
#include <cstring>
#include <chrono>
#include <clocale>
#include <ctime>
#include <algorithm>

RadioTUI::RadioTUI() {}

RadioTUI::~RadioTUI() {
    cleanup();
}

bool RadioTUI::init() {
    // Set locale for UTF-8 support
    setlocale(LC_ALL, "");

    // Initialize ncurses
    initscr();
    if (!stdscr) {
        fprintf(stderr, "Failed to initialize ncurses\n");
        return false;
    }

    // Set up terminal
    cbreak();
    noecho();
    curs_set(0);  // Hide cursor
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);  // Non-blocking input

    // Enable function keys and arrow keys
    set_escdelay(25);  // Reduce escape delay for faster key response

    if (has_colors()) {
        start_color();
        use_default_colors();
        setup_colors();
    }

    // Get dimensions
    getmaxyx(stdscr, max_y_, max_x_);

    // Check minimum terminal size
    if (max_x_ < 60 || max_y_ < 15) {
        endwin();
        fprintf(stderr, "Terminal too small. Minimum: 60x15, Current: %dx%d\n", max_x_, max_y_);
        return false;
    }

    // Adjust station width based on terminal size
    if (max_x_ >= 100) {
        station_width_ = 35;
    } else if (max_x_ >= 80) {
        station_width_ = 28;
    } else {
        station_width_ = 22;
    }

    create_windows();
    draw_all();

    return true;
}

void RadioTUI::cleanup() {
    destroy_windows();
    endwin();
}

void RadioTUI::setup_colors() {
    // Initialize color pairs
    init_pair(color_header_, COLOR_BLACK, COLOR_CYAN);
    init_pair(color_selected_, COLOR_BLACK, COLOR_GREEN);
    init_pair(color_border_, COLOR_BLUE, -1);
    init_pair(color_title_, COLOR_YELLOW, -1);
    init_pair(color_history_, COLOR_WHITE, -1);
    init_pair(color_controls_, COLOR_CYAN, -1);
    init_pair(color_history_num_, COLOR_CYAN, -1);
    init_pair(color_history_station_, COLOR_GREEN, -1);
    init_pair(color_history_time_, COLOR_BLUE, -1);
    init_pair(color_stopped_, COLOR_RED, -1);

    // Spectrum heat map colors
    init_pair(color_spectrum_low_, COLOR_GREEN, -1);    // Green for low intensity
    init_pair(color_spectrum_mid_, COLOR_YELLOW, -1);   // Yellow for medium
    init_pair(color_spectrum_high_, COLOR_RED, -1);     // Red for high intensity
}

void RadioTUI::create_windows() {
    getmaxyx(stdscr, max_y_, max_x_);

    header_win_ = newwin(1, max_x_, 0, 0);

    controls_win_ = newwin(1, max_x_, max_y_ - 1, 0);

    int content_height = max_y_ - 2;
    station_win_ = newwin(content_height, station_width_, 1, 0);

    int main_width = max_x_ - station_width_;
    main_win_ = newwin(content_height, main_width, 1, station_width_);
}

void RadioTUI::destroy_windows() {
    if (header_win_) delwin(header_win_);
    if (station_win_) delwin(station_win_);
    if (main_win_) delwin(main_win_);
    if (controls_win_) delwin(controls_win_);
    header_win_ = station_win_ = main_win_ = controls_win_ = nullptr;
}

void RadioTUI::set_stations(const std::vector<Station>& stations) {
    stations_ = stations;
    selected_station_ = 0;
    draw_stations();
}

void RadioTUI::update_metadata(const std::string& title, const std::string& station) {
    current_title_ = title;
    current_station_ = station;
    draw_main();
}

void RadioTUI::update_buffer(int percent) {
    buffer_percent_ = percent;
    draw_main();
}

void RadioTUI::set_playing(bool playing) {
    is_playing_ = playing;
    draw_all();
}

void RadioTUI::set_volume(int percent) {
    volume_percent_ = std::clamp(percent, 0, 100);
    draw_main();
}

void RadioTUI::update_stream_info(const std::string& format, int kbps) {
    if (!format.empty()) {
        stream_format_ = format;
    }
    if (kbps > 0) {
        stream_kbps_ = kbps;
    }
    draw_main();
}

void RadioTUI::update_stream_genre(const std::string& genre) {
    stream_genre_ = genre;
    draw_main();
}

void RadioTUI::update_track_metadata(const std::string& album, const std::string& year, const std::string& genre) {
    current_album_ = album;
    current_year_ = year;
    current_genre_ = genre;
    has_track_metadata_ = !album.empty() || !year.empty() || !genre.empty();
    draw_main();
}

void RadioTUI::update_spectrum(const std::array<float, FFTSpectrum::NUM_BARS>& bars) {
    spectrum_bars_ = bars;
    spectrum_updated_ = true;
    // Only redraw if playing to avoid unnecessary updates
    if (is_playing_) {
        draw_main();
    }
}

void RadioTUI::add_to_history(const std::string& title, const std::string& station) {
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        SongHistoryEntry entry;
        entry.title = title;
        entry.station_name = station;
        entry.played_at = std::chrono::system_clock::now();
        history_.push_back(entry);

        // Keep only last 15
        if (history_.size() > 15) {
            history_.erase(history_.begin());
        }
    }
    draw_main();
}

void RadioTUI::draw_all() {
    clear();
    refresh();

    draw_header();
    draw_stations();
    draw_main();
    draw_controls();
    refresh_all();

    refresh();
}

void RadioTUI::draw_header() {
    if (!header_win_) return;

    werase(header_win_);
    if (has_colors()) {
        wbkgd(header_win_, COLOR_PAIR(color_header_) | ' ');
    }

    std::string title = "Web Radio Player";
    std::string quit_hint = "[Quit: q]";

    wattron(header_win_, A_BOLD);
    mvwaddstr(header_win_, 0, 2, title.c_str());
    wattroff(header_win_, A_BOLD);
    mvwaddstr(header_win_, 0, max_x_ - quit_hint.length() - 2, quit_hint.c_str());
    wrefresh(header_win_);
}

void RadioTUI::draw_stations() {
    if (!station_win_) return;

    werase(station_win_);

    // Draw border
    wattron(station_win_, COLOR_PAIR(color_border_));
    box(station_win_, 0, 0);
    wattroff(station_win_, COLOR_PAIR(color_border_));

    // Title
    std::string title = " STATIONS ";
    mvwaddstr(station_win_, 0, (station_width_ - title.length()) / 2, title.c_str());

    // Station list
    int start_y = 2;
    int max_display = getmaxy(station_win_) - start_y - 2;

    for (size_t i = 0; i < stations_.size() && i < static_cast<size_t>(max_display); ++i) {
        int y = start_y + i;
        int x = 2;

        // Selection marker (cyan bold for selected, dim for others)
        if (i == selected_station_) {
            if (has_colors()) {
                wattron(station_win_, COLOR_PAIR(color_history_num_) | A_BOLD);
            }
            mvwaddstr(station_win_, y, x, "> ");
            x += 2;
            if (has_colors()) {
                wattroff(station_win_, COLOR_PAIR(color_history_num_) | A_BOLD);
            }
        } else {
            if (has_colors()) {
                wattron(station_win_, COLOR_PAIR(color_history_));
            }
            mvwaddstr(station_win_, y, x, "  ");
            x += 2;
            if (has_colors()) {
                wattroff(station_win_, COLOR_PAIR(color_history_));
            }
        }

        // Number (cyan bold)
        if (has_colors()) {
            wattron(station_win_, COLOR_PAIR(color_history_num_) | A_BOLD);
        }
        std::string number = std::to_string(i + 1) + ". ";
        mvwaddstr(station_win_, y, x, number.c_str());
        x += number.length();
        if (has_colors()) {
            wattroff(station_win_, COLOR_PAIR(color_history_num_) | A_BOLD);
        }

        // Station name (yellow bold, like song titles)
        if (has_colors()) {
            wattron(station_win_, COLOR_PAIR(color_title_) | A_BOLD);
        }
        std::string name = stations_[i].name;
        int max_name_len = station_width_ - x - 2;
        if (static_cast<int>(name.length()) > max_name_len) {
            name = name.substr(0, max_name_len - 3) + "...";
        }
        mvwaddstr(station_win_, y, x, name.c_str());
        if (has_colors()) {
            wattroff(station_win_, COLOR_PAIR(color_title_) | A_BOLD);
        }
    }



    wrefresh(station_win_);
}

void RadioTUI::draw_main() {
    if (!main_win_) return;

    werase(main_win_);

    // Draw border
    wattron(main_win_, COLOR_PAIR(color_border_));
    box(main_win_, 0, 0);
    wattroff(main_win_, COLOR_PAIR(color_border_));

    int y = 2;
    int max_x = getmaxx(main_win_);

    // Now Playing / Stopped section
    if (is_playing_) {
        std::string now_playing_title = " ♪ NOW PLAYING ♪ ";
        mvwaddstr(main_win_, y, (max_x - now_playing_title.length()) / 2, now_playing_title.c_str());
    } else {
        if (has_colors()) {
            wattron(main_win_, COLOR_PAIR(color_stopped_) | A_BOLD);
        }
        std::string stopped_title = " ■ STOPPED ";
        mvwaddstr(main_win_, y, (max_x - stopped_title.length()) / 2, stopped_title.c_str());
        if (has_colors()) {
            wattroff(main_win_, COLOR_PAIR(color_stopped_) | A_BOLD);
        }
    }
    y += 2;

    // Current song title
    if (!current_title_.empty()) {
        int x = 3;
        if (is_playing_) {
            wattron(main_win_, COLOR_PAIR(color_title_) | A_BOLD);
        } else {
            wattron(main_win_, COLOR_PAIR(color_history_));
        }
        std::string title = current_title_;
        int max_title_len = max_x - 6;
        if (static_cast<int>(title.length()) > max_title_len) {
            title = title.substr(0, max_title_len - 3) + "...";
        }
        mvwaddstr(main_win_, y, x, title.c_str());
        x += title.length();
        if (is_playing_) {
            wattroff(main_win_, COLOR_PAIR(color_title_) | A_BOLD);
        } else {
            wattroff(main_win_, COLOR_PAIR(color_history_));
        }

        // Album and year (if available)
        if (has_track_metadata_ && !current_album_.empty()) {
            if (has_colors()) {
                wattron(main_win_, COLOR_PAIR(color_history_));
            }
            mvwaddstr(main_win_, y, x, " — ");
            x += 3;
            if (has_colors()) {
                wattroff(main_win_, COLOR_PAIR(color_history_));
            }

            if (has_colors()) {
                wattron(main_win_, COLOR_PAIR(color_controls_));
            }
            std::string album_info = current_album_;
            if (!current_year_.empty()) {
                album_info += " (" + current_year_ + ")";
            }
            mvwaddstr(main_win_, y, x, album_info.c_str());
            if (has_colors()) {
                wattroff(main_win_, COLOR_PAIR(color_controls_));
            }
        }
        y += 2;

        // Genre (if available) - prefer MusicBrainz, fallback to stream
        std::string display_genre;
        if (has_track_metadata_ && !current_genre_.empty()) {
            display_genre = current_genre_;  // MusicBrainz genre (preferred)
        } else if (!stream_genre_.empty()) {
            display_genre = stream_genre_;   // Stream ICY genre (fallback)
        }

        if (!display_genre.empty()) {
            if (has_colors()) {
                wattron(main_win_, COLOR_PAIR(color_history_));
            }
            mvwaddstr(main_win_, y, 3, "Genre: ");
            if (has_colors()) {
                wattroff(main_win_, COLOR_PAIR(color_history_));
                wattron(main_win_, COLOR_PAIR(color_history_station_));
            }
            mvwaddstr(main_win_, y, 10, display_genre.c_str());
            if (has_colors()) {
                wattroff(main_win_, COLOR_PAIR(color_history_station_));
            }
            y += 1;
        }

        // Draw spectrum visualization on the right side
        if (is_playing_ && spectrum_updated_) {
            int max_x = getmaxx(main_win_);
            // Position spectrum starting at y=7 (moved down from title)
            draw_spectrum(7, max_x);
        }
    } else {
        if (!is_playing_) {
            mvwaddstr(main_win_, y, 3, "Select a station to start playing");
        } else {
            mvwaddstr(main_win_, y, 3, "Buffering...");
        }
        y += 2;
    }

    // Station name with stream info
    if (!current_station_.empty()) {
        int x = 3;

        // "Station:" label (dim)
        if (has_colors()) {
            wattron(main_win_, COLOR_PAIR(color_history_));
        }
        mvwaddstr(main_win_, y, x, "Station:");
        x += 9;
        if (has_colors()) {
            wattroff(main_win_, COLOR_PAIR(color_history_));
        }

        // Station name (green)
        if (has_colors()) {
            wattron(main_win_, COLOR_PAIR(color_history_station_));
        }
        std::string station = current_station_;
        int station_space = max_x - x - 30; // Reserve space for format and kbps
        if (static_cast<int>(station.length()) > station_space) {
            station = station.substr(0, station_space - 3) + "...";
        }
        mvwaddstr(main_win_, y, x, station.c_str());
        x += station.length();
        if (has_colors()) {
            wattroff(main_win_, COLOR_PAIR(color_history_station_));
        }

        // Stream format and KiB/s (if available)
        if (!stream_format_.empty() || stream_kbps_ > 0) {
            // Separator
            if (has_colors()) {
                wattron(main_win_, COLOR_PAIR(color_border_));
            }
            mvwaddstr(main_win_, y, x, " │ ");
            x += 3;
            if (has_colors()) {
                wattroff(main_win_, COLOR_PAIR(color_border_));
            }

            // Format (cyan)
            if (!stream_format_.empty()) {
                if (has_colors()) {
                    wattron(main_win_, COLOR_PAIR(color_controls_));
                }
                mvwaddstr(main_win_, y, x, stream_format_.c_str());
                x += stream_format_.length();
                if (has_colors()) {
                    wattroff(main_win_, COLOR_PAIR(color_controls_));
                }
            }

            // KiB/s (blue)
            if (stream_kbps_ > 0) {
                if (has_colors()) {
                    wattron(main_win_, COLOR_PAIR(color_history_time_));
                }
                std::string kbps_text = " " + std::to_string(stream_kbps_) + " KiB/s";
                mvwaddstr(main_win_, y, x, kbps_text.c_str());
                if (has_colors()) {
                    wattroff(main_win_, COLOR_PAIR(color_history_time_));
                }
            }
        }

        y += 2;
    }

    // Volume bar (always show, even when stopped)
    {
        int x = 3;

        // "Volume:" label (dim white)
        if (has_colors()) {
            wattron(main_win_, COLOR_PAIR(color_history_));
        }
        mvwaddstr(main_win_, y, x, "Volume: ");
        x += 8;

        // Opening bracket
        mvwaddstr(main_win_, y, x, "[");
        x += 1;

        // Bar with color based on volume level
        int bar_width = 20;
        int filled = (volume_percent_ * bar_width) / 100;

        for (int i = 0; i < bar_width; ++i) {
            if (has_colors()) {
                if (i < filled) {
                    // Yellow for volume
                    wattron(main_win_, COLOR_PAIR(color_title_));
                } else {
                    wattron(main_win_, COLOR_PAIR(color_history_));
                }
            }
            const char* block_char = (i < filled) ? "█" : "░";
            mvwaddstr(main_win_, y, x, block_char);
            x += 1;
            if (has_colors()) {
                wattroff(main_win_, COLOR_PAIR(color_title_));
                wattroff(main_win_, COLOR_PAIR(color_history_));
            }
        }

        // Closing bracket
        mvwaddstr(main_win_, y, x, "]");
        x += 1;

        // Percentage
        if (has_colors()) {
            wattron(main_win_, COLOR_PAIR(color_history_));
        }
        std::string vol_text = " " + std::to_string(volume_percent_) + "%";
        mvwaddstr(main_win_, y, x, vol_text.c_str());
        if (has_colors()) {
            wattroff(main_win_, COLOR_PAIR(color_history_));
        }

        y += 2;
    }

    // Buffer bar with label and gradient colors
    if (is_playing_) {
        int x = 3;

        // "Buffer:" label (dim white)
        if (has_colors()) {
            wattron(main_win_, COLOR_PAIR(color_history_));
        }
        mvwaddstr(main_win_, y, x, "Buffer: ");
        x += 8;
        if (has_colors()) {
            wattroff(main_win_, COLOR_PAIR(color_history_));
        }

        // Opening bracket
        mvwaddstr(main_win_, y, x, "[");
        x += 1;

        // Bar with gradient colors based on percentage
        int bar_width = 20;
        int filled = (buffer_percent_ * bar_width) / 100;

        for (int i = 0; i < bar_width; ++i) {
            if (has_colors()) {
                if (i < filled) {
                    // Gradient: bright cyan (0-33%), cyan (34-66%), green (67-100%)
                    int percent_position = (i * 100) / bar_width;
                    if (percent_position < 33) {
                        wattron(main_win_, COLOR_PAIR(color_controls_) | A_BOLD);
                    } else if (percent_position < 66) {
                        wattron(main_win_, COLOR_PAIR(color_controls_));
                    } else {
                        wattron(main_win_, COLOR_PAIR(color_history_station_));
                    }
                } else {
                    // Empty part - dim gray
                    wattron(main_win_, COLOR_PAIR(color_history_));
                }
            }
            const char* block_char = (i < filled) ? "█" : "░";
            mvwaddstr(main_win_, y, x, block_char);
            x += 1; // Screen position advances by 1 column
            if (has_colors()) {
                wattroff(main_win_, COLOR_PAIR(color_controls_) | A_BOLD);
                wattroff(main_win_, COLOR_PAIR(color_controls_));
                wattroff(main_win_, COLOR_PAIR(color_history_station_));
                wattroff(main_win_, COLOR_PAIR(color_history_));
            }
        }

        // Closing bracket
        mvwaddstr(main_win_, y, x, "]");
        x += 1;

        // Percentage (white)
        if (has_colors()) {
            wattron(main_win_, COLOR_PAIR(color_history_));
        }
        std::string percent_text = " " + std::to_string(buffer_percent_) + "%";
        mvwaddstr(main_win_, y, x, percent_text.c_str());
        if (has_colors()) {
            wattroff(main_win_, COLOR_PAIR(color_history_));
        }

        y += 2;
    }

    // Separator
    wattron(main_win_, COLOR_PAIR(color_border_));
    mvwhline(main_win_, y, 3, ACS_HLINE, max_x - 6);
    wattroff(main_win_, COLOR_PAIR(color_border_));
    y += 2;

        // History section
        std::string history_title = " HISTORY (Last 15) ";
        mvwaddstr(main_win_, y, (max_x - history_title.length()) / 2, history_title.c_str());
        y += 2;

        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            if (history_.empty()) {
                mvwaddstr(main_win_, y, 3, "No songs played yet.");
            } else {
                for (size_t i = 0; i < history_.size(); ++i) {
                    const auto& entry = history_[history_.size() - 1 - i];
                    int x = 3;

                    // Clock [HH:MM] (cyan bold)
                    if (has_colors()) {
                        wattron(main_win_, COLOR_PAIR(color_history_num_) | A_BOLD);
                    }
                    std::string clock_str = "[" + format_time_clock(entry.played_at) + "] ";
                    mvwaddstr(main_win_, y, x, clock_str.c_str());
                    x += clock_str.length();
                    if (has_colors()) {
                        wattroff(main_win_, COLOR_PAIR(color_history_num_) | A_BOLD);
                    }

                    // Song title (yellow, quoted)
                    if (has_colors()) {
                        wattron(main_win_, COLOR_PAIR(color_title_) | A_BOLD);
                    }
                    mvwaddstr(main_win_, y, x, "\"");
                    x += 1;

                    std::string title = entry.title;
                    int remaining = max_x - x - 25; // Reserve space for station, clock and ago time
                    if (static_cast<int>(title.length()) > remaining) {
                        title = title.substr(0, remaining - 3) + "...";
                    }
                    mvwaddstr(main_win_, y, x, title.c_str());
                    x += title.length();

                    mvwaddstr(main_win_, y, x, "\"");
                    x += 1;
                    if (has_colors()) {
                        wattroff(main_win_, COLOR_PAIR(color_title_) | A_BOLD);
                    }

                    // " on " (dim white)
                    if (!entry.station_name.empty()) {
                        if (has_colors()) {
                            wattron(main_win_, COLOR_PAIR(color_history_));
                        }
                        mvwaddstr(main_win_, y, x, " on ");
                        x += 4;
                        if (has_colors()) {
                            wattroff(main_win_, COLOR_PAIR(color_history_));
                        }

                        // Station name (green)
                        if (has_colors()) {
                            wattron(main_win_, COLOR_PAIR(color_history_station_));
                        }
                        std::string station = entry.station_name;
                        int station_space = max_x - x - 15; // Reserve space for ago time
                        if (static_cast<int>(station.length()) > station_space) {
                            station = station.substr(0, station_space - 3) + "...";
                        }
                        mvwaddstr(main_win_, y, x, station.c_str());
                        x += station.length();
                        if (has_colors()) {
                            wattroff(main_win_, COLOR_PAIR(color_history_station_));
                        }
                    }

                    // Time ago (blue)
                    if (has_colors()) {
                        wattron(main_win_, COLOR_PAIR(color_history_time_));
                    }
                    std::string time_str = " (" + format_time_ago(entry.played_at) + ")";
                    mvwaddstr(main_win_, y, x, time_str.c_str());
                    if (has_colors()) {
                        wattroff(main_win_, COLOR_PAIR(color_history_time_));
                    }

                    y++;
                }
            }
        }

    wrefresh(main_win_);
}

void RadioTUI::draw_spectrum(int y, int max_x) {
    if (!main_win_) return;

    // Spectrum dimensions - 50% wider bars, 20% higher (6 instead of 5)
    const int SPECTRUM_HEIGHT = 6;
    const int BAR_WIDTH = 2;  // Each bar is 2 characters wide
    const int BAR_SPACING = 1; // Small spacing between bars
    const int TOTAL_WIDTH = FFTSpectrum::NUM_BARS * BAR_WIDTH + (FFTSpectrum::NUM_BARS - 1) * BAR_SPACING;

    // Position: right-aligned with padding
    int start_x = max_x - TOTAL_WIDTH - 4;
    if (start_x < 25) start_x = 25;

    // Unicode block characters for smooth vertical gradient
    // ▁ ▂ ▃ ▄ ▅ ▆ ▇ █ (low to high)
    const char* gradient_blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    const int NUM_GRADIENTS = 8;

    for (int bar = 0; bar < FFTSpectrum::NUM_BARS; ++bar) {
        float magnitude = spectrum_bars_[bar];
        int bar_start_x = start_x + bar * (BAR_WIDTH + BAR_SPACING);

        // Ensure minimum bar height so it never disappears completely
        const float MIN_HEIGHT = 0.15f;
        float effective_magnitude = magnitude < MIN_HEIGHT ? MIN_HEIGHT : magnitude;

        // Calculate filled height
        float filled_height = effective_magnitude * SPECTRUM_HEIGHT;
        int full_rows = static_cast<int>(filled_height);
        float partial = filled_height - full_rows;
        int partial_idx = static_cast<int>(partial * NUM_GRADIENTS);

        // Draw from bottom to top
        for (int row = 0; row < SPECTRUM_HEIGHT; ++row) {
            int row_y = y + (SPECTRUM_HEIGHT - 1 - row);
            const char* block = " ";

            if (row < full_rows) {
                // Fully filled rows - solid block
                block = "█";
            } else if (row == full_rows && partial_idx > 0) {
                // Top partial row - use gradient block
                block = gradient_blocks[partial_idx - 1];
            }

            // Draw the block
            for (int w = 0; w < BAR_WIDTH; ++w) {
                // Color based on row height (heat map)
                if (has_colors()) {
                    if (row >= 4) {
                        wattron(main_win_, COLOR_PAIR(color_spectrum_high_) | A_BOLD);
                    } else if (row >= 2) {
                        wattron(main_win_, COLOR_PAIR(color_spectrum_mid_) | A_BOLD);
                    } else {
                        wattron(main_win_, COLOR_PAIR(color_spectrum_low_) | A_BOLD);
                    }
                    mvwaddstr(main_win_, row_y, bar_start_x + w, block);
                    wattroff(main_win_, COLOR_PAIR(color_spectrum_high_) | A_BOLD);
                    wattroff(main_win_, COLOR_PAIR(color_spectrum_mid_) | A_BOLD);
                    wattroff(main_win_, COLOR_PAIR(color_spectrum_low_) | A_BOLD);
                } else {
                    // Grayscale fallback
                    if (row < full_rows) {
                        mvwaddstr(main_win_, row_y, bar_start_x + w, "█");
                    } else if (row == full_rows && partial_idx > 0) {
                        mvwaddstr(main_win_, row_y, bar_start_x + w, "▒");
                    } else {
                        mvwaddstr(main_win_, row_y, bar_start_x + w, " ");
                    }
                }
            }
        }
    }
}

void RadioTUI::draw_controls() {
    if (!controls_win_) return;

    werase(controls_win_);

    int max_x = getmaxx(controls_win_);

    // Build the control sections
    struct ControlSection {
        const char* category;
        const char* keys;
    };

    ControlSection sections[] = {
        {"Stations", "[↑↓]/[Enter]"},
        {"Navigation", "[↑↓]"},
        {"Playback", "[Enter]/[s]"},
        {"Volume", "[+/-]"},
        {"Quick", "[1-9]"},
        {"Quit", "[q]"}
    };

    // Calculate total width needed
    int total_width = 0;
    for (const auto& sec : sections) {
        total_width += strlen(sec.category) + 1 + strlen(sec.keys) + 3; // +3 for " │ " separator
    }
    total_width -= 3; // Remove last separator

    // Start position to center the line
    int start_x = (max_x - total_width) / 2;
    if (start_x < 1) start_x = 1;

    int x = start_x;

    for (size_t i = 0; i < 6; ++i) {
        // Category label (dim)
        if (has_colors()) {
            wattron(controls_win_, COLOR_PAIR(color_history_));
        }
        mvwaddstr(controls_win_, 0, x, sections[i].category);
        x += strlen(sections[i].category);

        // Colon separator
        waddstr(controls_win_, ":");
        x += 1;

        // Keys (highlighted)
        if (has_colors()) {
            wattroff(controls_win_, COLOR_PAIR(color_history_));
            wattron(controls_win_, COLOR_PAIR(color_controls_) | A_BOLD);
        }
        mvwaddstr(controls_win_, 0, x, sections[i].keys);
        x += strlen(sections[i].keys);

        if (has_colors()) {
            wattroff(controls_win_, COLOR_PAIR(color_controls_) | A_BOLD);
        }

        // Separator between sections
        if (i < 5) {
            if (has_colors()) {
                wattron(controls_win_, COLOR_PAIR(color_border_));
            }
            waddstr(controls_win_, " │ ");
            x += 3;
            if (has_colors()) {
                wattroff(controls_win_, COLOR_PAIR(color_border_));
            }
        }
    }

    wrefresh(controls_win_);
}

void RadioTUI::refresh_all() {
    // Force immediate update of all windows
    if (header_win_) wrefresh(header_win_);
    if (station_win_) wrefresh(station_win_);
    if (main_win_) wrefresh(main_win_);
    if (controls_win_) wrefresh(controls_win_);
    refresh();
}

int RadioTUI::get_input() {
    return wgetch(stdscr);
}

void RadioTUI::handle_input(int ch) {
    // Debug: show key code (remove after testing)
    // mvprintw(0, 0, "Key: %d (%c)", ch, ch);
    // refresh();

    switch (ch) {
        case KEY_UP:
        case 'k':
        case 'K':
            prev_station();
            break;

        case KEY_DOWN:
        case 'j':
        case 'J':
            next_station();
            break;

        case '\n':
        case '\r':
        case KEY_ENTER:
            select_station();
            break;

        case 's':
        case 'S':
            if (on_stop_) on_stop_();
            break;

        case 'q':
        case 'Q':
            if (on_quit_) on_quit_();
            break;

        case '+':
        case '=':
        case ']':
            if (on_volume_up_) on_volume_up_();
            break;

        case '-':
        case '[':
            if (on_volume_down_) on_volume_down_();
            break;

        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            if (ch - '1' < static_cast<int>(stations_.size())) {
                selected_station_ = ch - '1';
                draw_stations();
                select_station();
            }
            break;

        case KEY_RESIZE:
            // Handle terminal resize
            endwin();
            refresh();
            getmaxyx(stdscr, max_y_, max_x_);
            destroy_windows();
            create_windows();
            draw_all();
            break;
    }
}

void RadioTUI::next_station() {
    if (stations_.empty()) return;
    selected_station_ = (selected_station_ + 1) % stations_.size();
    draw_stations();
}

void RadioTUI::prev_station() {
    if (stations_.empty()) return;
    selected_station_ = (selected_station_ + stations_.size() - 1) % stations_.size();
    draw_stations();
}

void RadioTUI::select_station() {
    if (selected_station_ < stations_.size() && on_station_select_) {
        on_station_select_(stations_[selected_station_]);
    }
}

void RadioTUI::set_on_station_select(std::function<void(const Station&)> cb) {
    on_station_select_ = cb;
}

void RadioTUI::set_on_stop(std::function<void()> cb) {
    on_stop_ = cb;
}

void RadioTUI::set_on_quit(std::function<void()> cb) {
    on_quit_ = cb;
}

void RadioTUI::set_on_volume_up(std::function<void()> cb) {
    on_volume_up_ = cb;
}

void RadioTUI::set_on_volume_down(std::function<void()> cb) {
    on_volume_down_ = cb;
}

std::string RadioTUI::format_time_ago(const std::chrono::system_clock::time_point& tp) {
    auto elapsed = std::chrono::system_clock::now() - tp;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(elapsed).count();

    if (minutes < 1) return "now";
    if (minutes < 60) return std::to_string(minutes) + "m";
    auto hours = minutes / 60;
    return std::to_string(hours) + "h";
}

std::string RadioTUI::format_time_clock(const std::chrono::system_clock::time_point& tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
    std::tm local_tm = *std::localtime(&time);
    char buf[6];
    std::strftime(buf, sizeof(buf), "%H:%M", &local_tm);
    return std::string(buf);
}
