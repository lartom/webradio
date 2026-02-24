#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <cstring>
#include <chrono>
#include <memory>
#include <functional>

#include <nlohmann/json.hpp>
#include <fstream>

#include "byte_ringbuffer.hpp"
#include "tui.hpp"
#include "fft_spectrum.hpp"

using namespace std::chrono_literals;

// FFmpeg logging control - define FFMPEG_DEBUG_LOGGING to enable
#ifndef FFMPEG_DEBUG_LOGGING
static void suppress_ffmpeg_logging(void* ptr, int level, const char* fmt, va_list vl) {
    (void)ptr;
    (void)level;
    (void)fmt;
    (void)vl;
    // Suppress all FFmpeg logging
}
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

using json = nlohmann::json;

// Global state
std::atomic<bool> g_running{true};
std::atomic<bool> g_playing{false};
std::atomic<float> g_volume{1.0f};  // 0.0 to 1.0
std::string g_current_metadata;
std::string g_current_station_name;

// Thread-safe metadata updates for TUI (ncurses is not thread-safe)
struct PendingMetadataUpdate {
    std::string title;
    std::string station;
    bool pending = false;
};
std::mutex g_metadata_mutex;
PendingMetadataUpdate g_pending_metadata;

// Thread-safe buffer status updates
std::atomic<int> g_pending_buffer_percent{-1};  // -1 means no update
std::atomic<bool> g_pending_playing_state{false};
std::atomic<bool> g_has_playing_state_update{false};

// Thread-safe stream info updates
struct PendingStreamInfo {
    std::string format;
    int kbps = 0;
    bool pending = false;
};
std::mutex g_stream_info_mutex;
PendingStreamInfo g_pending_stream_info;

// Thread-safe stream genre updates
std::atomic<bool> g_pending_genre_update{false};
std::string g_pending_genre;

// Real-time bandwidth tracking
std::atomic<uint64_t> g_bytes_received{0};
std::atomic<uint64_t> g_last_bandwidth_update{0};
std::atomic<int> g_current_kbps{0};

#ifdef ENABLE_MUSICBRAINZ
// MusicBrainz metadata fetcher
std::unique_ptr<metadata::MusicBrainzFetcher> g_metadata_fetcher;
std::string g_current_artist;
std::string g_current_song_title;
#endif

// TUI instance
std::unique_ptr<RadioTUI> g_tui;

// FFT spectrum analyzer
std::unique_ptr<FFTSpectrum> g_fft_spectrum;

void signal_handler(int) {
    g_running = false;
}

void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    
    ByteRingbuffer* buffer = static_cast<ByteRingbuffer*>(pDevice->pUserData);
    if (!buffer) return;

    uint8_t* output = static_cast<uint8_t*>(pOutput);
    constexpr size_t bytesPerFrame = 4;
    size_t bytesToWrite = static_cast<size_t>(frameCount) * bytesPerFrame;
    
    size_t bytesRead = buffer->read(output, bytesToWrite);
    
    if (bytesRead < bytesToWrite) {
        std::memset(output + bytesRead, 0, bytesToWrite - bytesRead);
    }
    
    // Apply volume scaling to 16-bit stereo samples
    if (bytesRead > 0) {
        float volume = g_volume.load();
        if (volume < 0.99f) {
            int16_t* samples = reinterpret_cast<int16_t*>(output);
            size_t sampleCount = bytesRead / 2;
            for (size_t i = 0; i < sampleCount; ++i) {
                samples[i] = static_cast<int16_t>(samples[i] * volume);
            }
        }
    }
    
    // Feed samples to FFT spectrum analyzer
    if (g_fft_spectrum && bytesRead > 0) {
        const int16_t* samples = reinterpret_cast<const int16_t*>(output);
        g_fft_spectrum->process_samples(samples, bytesRead / bytesPerFrame);
    }
}

std::vector<Station> load_stations(const std::string& filename) {
    std::vector<Station> stations;
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        return stations;
    }
    
    json j;
    file >> j;
    
    for (auto& [name, url] : j.items()) {
        stations.push_back({name, url.get<std::string>()});
    }
    
    return stations;
}

class AudioPlayer {
private:
    std::thread playback_thread_;
    std::atomic<bool> stop_requested_{false};
    std::string current_url_;
    ByteRingbuffer audio_buffer_;
    
public:
    void play(const std::string& url, const std::string& station_name) {
        stop();
        current_url_ = url;
        g_current_station_name = station_name;
        stop_requested_ = false;
        g_playing = true;
        
        // Signal main thread to update TUI (ncurses is not thread-safe)
        g_pending_playing_state = true;
        g_has_playing_state_update = true;
        
        playback_thread_ = std::thread([this]() {
            play_stream(current_url_);
        });
    }
    
    void stop() {
        stop_requested_ = true;
        g_playing = false;
        
        if (playback_thread_.joinable()) {
            playback_thread_.join();
        }
        
        // Signal main thread to update TUI (ncurses is not thread-safe)
        g_pending_playing_state = false;
        g_has_playing_state_update = true;
    }
    
    bool is_playing() const {
        return g_playing;
    }

private:
    void update_metadata_tui(AVFormatContext* fmt_ctx, int audio_stream_idx) {
        AVDictionaryEntry* tag = nullptr;
        bool metadata_updated = false;
        std::string new_title;
        
        // Helper lambda to check metadata in both stream and format contexts
        auto check_metadata = [&](const char* key) -> AVDictionaryEntry* {
            AVDictionaryEntry* t = nullptr;
            // First check stream-level metadata (for ICY)
            if (audio_stream_idx >= 0 && fmt_ctx->streams[audio_stream_idx]) {
                t = av_dict_get(fmt_ctx->streams[audio_stream_idx]->metadata, key, nullptr, 0);
            }
            // Fallback to format-level metadata
            if (!t) {
                t = av_dict_get(fmt_ctx->metadata, key, nullptr, 0);
            }
            return t;
        };
        
        // Check for ICY metadata (StreamTitle)
        tag = check_metadata("StreamTitle");
        if (tag && tag->value) {
            new_title = tag->value;
            if (new_title.length() > 2 && new_title.front() == '\'' && new_title.back() == '\'') {
                new_title = new_title.substr(1, new_title.length() - 2);
            }
            if (!new_title.empty() && new_title != g_current_metadata) {
                g_current_metadata = new_title;
                metadata_updated = true;
            }
        }
        
        // Check for TITLE
        if (!metadata_updated) {
            tag = check_metadata("TITLE");
            if (tag && tag->value && strlen(tag->value) > 0) {
                new_title = tag->value;
                if (new_title != g_current_metadata) {
                    g_current_metadata = new_title;
                    metadata_updated = true;
                }
            }
        }
        
        // Check for artist/title combination
        std::string artist;
        std::string title;
        
        if (!metadata_updated) {
            AVDictionaryEntry* artist_tag = check_metadata("artist");
            AVDictionaryEntry* title_tag = check_metadata("title");
            
            if (artist_tag && title_tag && artist_tag->value && title_tag->value &&
                strlen(artist_tag->value) > 0 && strlen(title_tag->value) > 0) {
                artist = artist_tag->value;
                title = title_tag->value;
                new_title = artist + " - " + title;
                if (new_title != g_current_metadata) {
                    g_current_metadata = new_title;
                    metadata_updated = true;
                }
            }
        }
        
        // If we have a title but no artist, try to parse from ICY format "Artist - Title"
        if (metadata_updated && artist.empty() && !new_title.empty()) {
            size_t separator_pos = new_title.find(" - ");
            if (separator_pos != std::string::npos) {
                artist = new_title.substr(0, separator_pos);
                title = new_title.substr(separator_pos + 3);
            } else {
                title = new_title;
            }
        }
        
        if (metadata_updated) {
            // Queue metadata update for main thread to handle (ncurses is not thread-safe)
            std::lock_guard<std::mutex> lock(g_metadata_mutex);
            g_pending_metadata.title = g_current_metadata;
            
#ifdef ENABLE_MUSICBRAINZ
            // Store artist/title for metadata fetching
            g_current_artist = artist;
            g_current_song_title = title;
            
            // Request metadata from MusicBrainz
            if (g_metadata_fetcher) {
                g_metadata_fetcher->request(artist, title);
            }
#endif
            g_pending_metadata.station = g_current_station_name;
            g_pending_metadata.pending = true;
            
            // Extract and send stream genre if available
            if (fmt_ctx) {
                AVDictionaryEntry* genre_tag = nullptr;
                
                // Look for genre in stream-level metadata first (ICY)
                if (audio_stream_idx >= 0 && fmt_ctx->streams[audio_stream_idx]) {
                    genre_tag = av_dict_get(fmt_ctx->streams[audio_stream_idx]->metadata, "genre", nullptr, 0);
                    if (!genre_tag) {
                        genre_tag = av_dict_get(fmt_ctx->streams[audio_stream_idx]->metadata, "icy-genre", nullptr, 0);
                    }
                }
                
                // Fallback to format-level metadata
                if (!genre_tag) {
                    genre_tag = av_dict_get(fmt_ctx->metadata, "genre", nullptr, 0);
                }
                if (!genre_tag) {
                    genre_tag = av_dict_get(fmt_ctx->metadata, "icy-genre", nullptr, 0);
                }
                
                if (genre_tag && genre_tag->value) {
                    g_pending_genre = genre_tag->value;
                    g_pending_genre_update = true;
                }
            }
            
#ifdef METADATA_DEBUG_VIEW
            // Queue debug metadata for main thread (ncurses is not thread-safe)
            if (g_tui && fmt_ctx) {
                g_tui->clear_debug_metadata();
                AVDictionaryEntry* tag = nullptr;
                
                // Format-level metadata
                while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
                    std::string key = std::string("[FMT] ") + tag->key;
                    g_tui->queue_debug_metadata(key, tag->value);
                }
                
                // Stream-level metadata
                tag = nullptr;
                if (audio_stream_idx >= 0 && fmt_ctx->streams[audio_stream_idx]) {
                    while ((tag = av_dict_get(fmt_ctx->streams[audio_stream_idx]->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
                        std::string key = std::string("[STR] ") + tag->key;
                        g_tui->queue_debug_metadata(key, tag->value);
                    }
                }
            }
#endif
        }
    }
    
    bool play_stream(const std::string& url) {
        AVFormatContext* fmt_ctx = nullptr;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "icy", "1", 0);
        
        int ret = avformat_open_input(&fmt_ctx, url.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        
        if (ret < 0) {
            return false;
        }
        
        ret = avformat_find_stream_info(fmt_ctx, nullptr);
        if (ret < 0) {
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        g_current_metadata = "";
        
        int audio_stream_idx = -1;
        const AVCodec* codec = nullptr;
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
            AVStream* stream = fmt_ctx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream_idx = i;
                codec = avcodec_find_decoder(stream->codecpar->codec_id);
                break;
            }
        }
        
        if (audio_stream_idx == -1 || !codec) {
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        ret = avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[audio_stream_idx]->codecpar);
        if (ret < 0) {
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        ret = avcodec_open2(codec_ctx, codec, nullptr);
        if (ret < 0) {
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        // Get stream format info
        {
            std::lock_guard<std::mutex> lock(g_stream_info_mutex);
            // Format: "CODEC BITRATEkbps" (e.g., "MP3 128kbps")
            std::string codec_name = codec->name;
            // Capitalize first letter
            if (!codec_name.empty()) {
                codec_name[0] = std::toupper(codec_name[0]);
            }
            
            int bitrate_kbps = codec_ctx->bit_rate / 1000;
            if (bitrate_kbps > 0) {
                g_pending_stream_info.format = codec_name + " " + std::to_string(bitrate_kbps) + "kbps";
            } else {
                g_pending_stream_info.format = codec_name;
            }
            g_pending_stream_info.pending = true;
        }
        
        SwrContext* swr_ctx = swr_alloc();
        if (!swr_ctx) {
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        AVChannelLayout out_ch_layout;
        av_channel_layout_default(&out_ch_layout, 2);
        
        ret = swr_alloc_set_opts2(&swr_ctx,
            &out_ch_layout,
            AV_SAMPLE_FMT_S16,
            44100,
            &codec_ctx->ch_layout,
            codec_ctx->sample_fmt,
            codec_ctx->sample_rate,
            0, nullptr);
        
        if (ret < 0) {
            swr_free(&swr_ctx);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        ret = swr_init(swr_ctx);
        if (ret < 0) {
            swr_free(&swr_ctx);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format = ma_format_s16;
        deviceConfig.playback.channels = 2;
        deviceConfig.sampleRate = 44100;
        deviceConfig.dataCallback = data_callback;
        deviceConfig.pUserData = &audio_buffer_;
        
        ma_device device;
        ret = ma_device_init(nullptr, &deviceConfig, &device);
        if (ret != MA_SUCCESS) {
            swr_free(&swr_ctx);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        
        if (!packet || !frame) {
            ma_device_uninit(&device);
            swr_free(&swr_ctx);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        // Pre-buffering
        audio_buffer_.consumerClear();
        constexpr size_t PREBUFFER_TARGET = 65536;
        
        while (!stop_requested_ && audio_buffer_.readAvailable() < PREBUFFER_TARGET) {
            ret = av_read_frame(fmt_ctx, packet);
            if (ret < 0) break;
            
            if (packet->stream_index == audio_stream_idx) {
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret < 0) {
                    av_packet_unref(packet);
                    continue;
                }
                
                while (ret >= 0) {
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;
                    
                    int out_samples = swr_get_out_samples(swr_ctx, frame->nb_samples);
                    if (out_samples <= 0) continue;
                    
                    alignas(16) uint8_t temp_buffer[32768];
                    int max_samples = static_cast<int>(sizeof(temp_buffer) / 4);
                    
                    uint8_t* out_buf = temp_buffer;
                    int converted_samples = swr_convert(swr_ctx,
                        &out_buf, max_samples,
                        (const uint8_t**)frame->data, frame->nb_samples);
                    
                    if (converted_samples > 0) {
                        size_t data_size = static_cast<size_t>(converted_samples) * 4;
                        size_t written = 0;
                        while (written < data_size && !stop_requested_) {
                            size_t n = audio_buffer_.write(temp_buffer + written, data_size - written);
                            if (n == 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            } else {
                                written += n;
                            }
                        }
                    }
                }
            }
            av_packet_unref(packet);
            
            // Update buffer percent for main thread (ncurses is not thread-safe)
            size_t filled = audio_buffer_.readAvailable();
            int percent = static_cast<int>((filled * 100) / PREBUFFER_TARGET);
            if (percent > 100) percent = 100;
            g_pending_buffer_percent = percent;
        }
        
        if (stop_requested_) {
            av_packet_free(&packet);
            av_frame_free(&frame);
            ma_device_uninit(&device);
            swr_free(&swr_ctx);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return true;
        }
        
        ret = ma_device_start(&device);
        if (ret != MA_SUCCESS) {
            av_packet_free(&packet);
            av_frame_free(&frame);
            ma_device_uninit(&device);
            swr_free(&swr_ctx);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return false;
        }
        
        // Main playback loop
        int metadata_counter = 0;
        uint64_t bytes_accumulated = 0;
        auto last_calc = std::chrono::steady_clock::now();
        
        while (!stop_requested_) {
            ret = av_read_frame(fmt_ctx, packet);
            if (ret < 0) break;
            
            // Track bandwidth
            bytes_accumulated += packet->size;
            
            // Calculate KiB/s every second
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_calc).count();
            if (elapsed >= 1000) {
                int kbps = static_cast<int>((bytes_accumulated * 1000) / (elapsed * 1024));
                g_current_kbps.store(kbps);

                // Queue stream info update for main thread (ncurses is not thread-safe)
                {
                    std::lock_guard<std::mutex> lock(g_stream_info_mutex);
                    g_pending_stream_info.kbps = kbps;
                    g_pending_stream_info.pending = true;
                }

#ifdef METADATA_DEBUG_VIEW
                // Queue debug metadata for main thread (ncurses is not thread-safe)
                if (g_tui) {
                    g_tui->clear_debug_metadata();
                    AVDictionaryEntry* tag = nullptr;

                    // Format-level metadata
                    while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
                        std::string key = std::string("[FMT] ") + tag->key;
                        g_tui->queue_debug_metadata(key, tag->value);
                    }

                    // Stream-level metadata
                    tag = nullptr;
                    if (audio_stream_idx >= 0 && fmt_ctx->streams[audio_stream_idx]) {
                        while ((tag = av_dict_get(fmt_ctx->streams[audio_stream_idx]->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
                            std::string key = std::string("[STR] ") + tag->key;
                            g_tui->queue_debug_metadata(key, tag->value);
                        }
                    }
                }
#endif

                bytes_accumulated = 0;
                last_calc = now;
            }
            
            // Check metadata frequently (every 5 packets for ICY metadata)
            if (++metadata_counter % 5 == 0) {
                update_metadata_tui(fmt_ctx, audio_stream_idx);
            }
            
            if (packet->stream_index == audio_stream_idx) {
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret < 0) {
                    av_packet_unref(packet);
                    continue;
                }
                
                while (ret >= 0) {
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;
                    
                    int out_samples = swr_get_out_samples(swr_ctx, frame->nb_samples);
                    if (out_samples <= 0) continue;
                    
                    alignas(16) uint8_t temp_buffer[32768];
                    int max_samples = static_cast<int>(sizeof(temp_buffer) / 4);
                    
                    uint8_t* out_buf = temp_buffer;
                    int converted_samples = swr_convert(swr_ctx,
                        &out_buf, max_samples,
                        (const uint8_t**)frame->data, frame->nb_samples);
                    
                    if (converted_samples > 0) {
                        size_t data_size = static_cast<size_t>(converted_samples) * 4;
                        size_t written = 0;
                        while (written < data_size && !stop_requested_) {
                            size_t n = audio_buffer_.write(temp_buffer + written, data_size - written);
                            if (n == 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            } else {
                                written += n;
                            }
                        }
                    }
                }
            }
            av_packet_unref(packet);
        }

        av_packet_free(&packet);
        av_frame_free(&frame);
        ma_device_stop(&device);
        ma_device_uninit(&device);
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);

        audio_buffer_.consumerClear();
        return true;
    }
};

int main(int argc, char* argv[]) {
    // Disable FFmpeg logging unless FFMPEG_DEBUG_LOGGING is defined
#ifndef FFMPEG_DEBUG_LOGGING
    av_log_set_callback(suppress_ffmpeg_logging);
#endif

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::string stations_file = "stations.json";
    if (argc > 1) {
        stations_file = argv[1];
    }
    
    std::vector<Station> stations = load_stations(stations_file);
    if (stations.empty()) {
        std::cerr << "No stations loaded" << std::endl;
        return 1;
    }
    
    // Initialize TUI
    g_tui = std::make_unique<RadioTUI>();
    if (!g_tui->init()) {
        std::cerr << "Failed to initialize TUI" << std::endl;
        return 1;
    }
    
    // Initialize FFT spectrum analyzer
    g_fft_spectrum = std::make_unique<FFTSpectrum>();
    
#ifdef ENABLE_MUSICBRAINZ
    // Initialize MusicBrainz metadata fetcher
    g_metadata_fetcher = std::make_unique<metadata::MusicBrainzFetcher>();
#ifdef METADATA_DEBUG_VIEW
    // Set up status callback for debug view
    g_metadata_fetcher->setStatusCallback([](const std::string& status,
                                             const std::string& query,
                                             const std::string& album,
                                             const std::string& year,
                                             const std::string& genre,
                                             int score,
                                             bool has_result,
                                             const std::string& error_message) {
        if (g_tui) {
            g_tui->update_musicbrainz_debug(status, query, album, year, genre, score, has_result, error_message);
        }
    });
#endif
    g_metadata_fetcher->start();
#endif
    
    g_tui->set_stations(stations);
    
    // Create audio player
    AudioPlayer player;
    
    // Set up callbacks
    g_tui->set_on_station_select([&player, &stations](const Station& station) {
#ifdef ENABLE_MUSICBRAINZ
        // Clear previous track metadata when selecting new station
        if (g_tui) {
            g_tui->update_track_metadata("", "", "");
        }
        g_current_artist.clear();
        g_current_song_title.clear();
#endif
        // Clear stream genre
        if (g_tui) {
            g_tui->update_stream_genre("");
        }
#ifdef METADATA_DEBUG_VIEW
        // Clear debug metadata
        if (g_tui) {
            g_tui->clear_debug_metadata();
            g_tui->update_musicbrainz_debug("Waiting...");
        }
#endif
        player.play(station.url, station.name);
    });
    
    g_tui->set_on_stop([&player]() {
        player.stop();
#ifdef ENABLE_MUSICBRAINZ
        // Clear current track metadata
        g_current_artist.clear();
        g_current_song_title.clear();
        if (g_tui) {
            g_tui->update_track_metadata("", "", "");
        }
#endif
        // Clear stream genre
        if (g_tui) {
            g_tui->update_stream_genre("");
        }
    });
    
    g_tui->set_on_quit([]() {
        g_running = false;
    });
    
    g_tui->set_on_volume_up([]() {
        float vol = g_volume.load();
        vol = std::min(vol + 0.05f, 1.0f);
        g_volume.store(vol);
        if (g_tui) {
            g_tui->set_volume(static_cast<int>(vol * 100));
        }
    });
    
    g_tui->set_on_volume_down([]() {
        float vol = g_volume.load();
        vol = std::max(vol - 0.05f, 0.0f);
        g_volume.store(vol);
        if (g_tui) {
            g_tui->set_volume(static_cast<int>(vol * 100));
        }
    });
    
    // Initial draw
    g_tui->draw_all();
    
    // Main event loop
    while (g_running) {
        int ch = g_tui->get_input();
        if (ch != ERR) {
            g_tui->handle_input(ch);
        }
        
        // Check for pending TUI updates (must be done in main thread for ncurses safety)
        {
            // Handle playing state changes
            if (g_has_playing_state_update && g_tui) {
                g_tui->set_playing(g_pending_playing_state);
                g_has_playing_state_update = false;
            }
            
            // Handle buffer updates
            int buffer_percent = g_pending_buffer_percent.load();
            if (buffer_percent >= 0 && g_tui) {
                g_tui->update_buffer(buffer_percent);
                g_pending_buffer_percent = -1;  // Reset
            }
            
            // Handle metadata updates
            std::string meta_title;
            std::string meta_station;
            bool has_meta_update = false;
            
            {
                std::lock_guard<std::mutex> lock(g_metadata_mutex);
                if (g_pending_metadata.pending) {
                    meta_title = g_pending_metadata.title;
                    meta_station = g_pending_metadata.station;
                    g_pending_metadata.pending = false;
                    has_meta_update = true;
                }
            }
            
            if (has_meta_update && g_tui) {
#ifdef ENABLE_MUSICBRAINZ
                // Clear previous track metadata when new song starts
                g_tui->update_track_metadata("", "", "");
#endif
                g_tui->update_metadata(meta_title, meta_station);
                g_tui->add_to_history(meta_title, meta_station);
            }
            
            // Handle stream info updates
            std::string stream_format;
            int stream_kbps = 0;
            bool has_stream_update = false;
            
            {
                std::lock_guard<std::mutex> lock(g_stream_info_mutex);
                if (g_pending_stream_info.pending) {
                    stream_format = g_pending_stream_info.format;
                    stream_kbps = g_pending_stream_info.kbps;
                    g_pending_stream_info.pending = false;
                    has_stream_update = true;
                }
            }
            
            if (has_stream_update && g_tui) {
                g_tui->update_stream_info(stream_format, stream_kbps);
            }
            
            // Handle genre updates
            if (g_pending_genre_update && g_tui) {
                g_tui->update_stream_genre(g_pending_genre);
                g_pending_genre_update = false;
            }
            
#ifdef ENABLE_MUSICBRAINZ
            // Check for MusicBrainz metadata results
            if (g_metadata_fetcher && g_tui && !g_current_artist.empty() && !g_current_song_title.empty()) {
                if (g_metadata_fetcher->hasResult(g_current_artist, g_current_song_title)) {
                    auto track_info = g_metadata_fetcher->getResult(g_current_artist, g_current_song_title);
                    if (track_info.available) {
                        g_tui->update_track_metadata(track_info.album, track_info.year, track_info.genre);
                    }
                }
            }
#endif

#ifdef METADATA_DEBUG_VIEW
            // Process pending debug metadata from playback thread
            if (g_tui) {
                g_tui->process_pending_debug_metadata();
            }
            
            // Check if debug view needs redraw
            if (g_tui && g_tui->needsDebugRedraw()) {
                g_tui->draw_debug();
            }
#endif
            
            // Handle spectrum visualization updates
            if (g_fft_spectrum && g_fft_spectrum->has_new_data() && g_tui) {
                std::array<float, FFTSpectrum::NUM_BARS> spectrum_bars;
                bool updated = false;
                g_fft_spectrum->get_spectrum(spectrum_bars, updated);
                if (updated) {
                    g_tui->update_spectrum(spectrum_bars);
                }
            }
        }

        // Small sleep to prevent CPU spinning
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Cleanup
    player.stop();
    
#ifdef ENABLE_MUSICBRAINZ
    // Stop metadata fetcher
    if (g_metadata_fetcher) {
        g_metadata_fetcher->stop();
        g_metadata_fetcher.reset();
    }
#endif
    
    g_tui->cleanup();
    
    return 0;
}
