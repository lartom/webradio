#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <cstring>
#include <chrono>
#include <memory>
#include <functional>
#include <filesystem>
#include <array>
#include <cstdlib>

#include <nlohmann/json.hpp>
#include <fstream>

#ifdef __linux__
#include <unistd.h>
#include <limits.h>
#endif

#include "byte_ringbuffer.hpp"
#include "tui.hpp"
#include "fft_spectrum.hpp"

#ifdef WEBRADIO_USE_SSE2
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <emmintrin.h>
#endif
#endif

using namespace std::chrono_literals;

#ifndef FFMPEG_DEBUG_LOGGING
static void suppress_ffmpeg_logging(void* ptr, int level, const char* fmt, va_list vl) {
    (void)ptr;
    (void)level;
    (void)fmt;
    (void)vl;
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

#ifndef WEBRADIO_VERSION
#define WEBRADIO_VERSION "0.0.0"
#endif

std::atomic<bool> g_running{true};
std::atomic<bool> g_playing{false};
std::atomic<float> g_volume{1.0f};
std::string g_current_metadata;
std::string g_current_station_name;


uint64_t g_bytes_accumulated = 0;
auto g_last_kbps_calc = std::chrono::steady_clock::now();

std::mutex g_metadata_mutex;
StreamMetadata g_pending_metadata;

std::atomic<int> g_pending_buffer_percent{-1};
std::atomic<bool> g_pending_playing_state{false};
std::atomic<bool> g_has_playing_state_update{false};


//std::atomic<bool> g_pending_genre_update{false};
//std::string g_pending_genre;

std::atomic<uint64_t> g_bytes_received{0};
std::atomic<uint64_t> g_last_bandwidth_update{0};

std::unique_ptr<RadioTUI> g_tui;
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
   
	// copy PCM data to audio playback buffer
    size_t bytesRead = 0;
    while (bytesRead < bytesToWrite) {
        const uint8_t* src = nullptr;
        size_t available = buffer->reserve_read_contiguous(src);
        if (available == 0 || src == nullptr) {
            break;
        }

        size_t chunk = std::min(available, bytesToWrite - bytesRead);
        std::memcpy(output + bytesRead, src, chunk);
        buffer->consume(chunk);
        bytesRead += chunk;
    }

	if (bytesRead > 0) {
		float volume = g_volume.load();
		if (volume <= 0.99f) {
			int16_t* samples = reinterpret_cast<int16_t*>(output);
			size_t sampleCount = bytesRead / 2;

#ifdef WEBRADIO_USE_SSE2
			// SSE2: process 8 x int16 per iteration
			// int16 -> int32 -> float -> scale -> int32 -> int16 (saturated)
			const __m128 vol_vec = _mm_set1_ps(volume);
			size_t i = 0;
			for (; i + 7 < sampleCount; i += 8) {
				__m128i s16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&samples[i]));
				// Sign-extend int16 -> int32 using arithmetic shift to fill upper bits
				__m128i sign = _mm_srai_epi16(s16, 15);
				__m128i lo_i32 = _mm_unpacklo_epi16(s16, sign);
				__m128i hi_i32 = _mm_unpackhi_epi16(s16, sign);
				// Convert to float, scale, convert back, pack with saturation
				__m128i result = _mm_packs_epi32(
					_mm_cvtps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(lo_i32), vol_vec)),
					_mm_cvtps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(hi_i32), vol_vec)));
				_mm_storeu_si128(reinterpret_cast<__m128i*>(&samples[i]), result);
			}
			// Scalar tail for remaining samples
			for (; i < sampleCount; ++i) {
				samples[i] = static_cast<int16_t>(samples[i] * volume);
			}
#else
			for (size_t i = 0; i < sampleCount; ++i) {
				samples[i] = static_cast<int16_t>(samples[i] * volume);
			}
#endif
		}
	}
    
    if (g_fft_spectrum && bytesRead > 0) {
        const int16_t* samples = reinterpret_cast<const int16_t*>(output);
        g_fft_spectrum->push_samples(samples, bytesRead / bytesPerFrame);
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

std::string get_executable_directory() {
#ifdef __linux__
    std::array<char, PATH_MAX> exe_path{};
    ssize_t len = readlink("/proc/self/exe", exe_path.data(), exe_path.size() - 1);
    if (len <= 0) {
        return {};
    }

    exe_path[static_cast<size_t>(len)] = '\0';
    return std::filesystem::path(exe_path.data()).parent_path().string();
#else
    return {};
#endif
}

std::string resolve_default_stations_file() {
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(std::filesystem::current_path() / "stations.json");

    std::string exe_dir = get_executable_directory();
    if (!exe_dir.empty()) {
        candidates.emplace_back(std::filesystem::path(exe_dir) / "stations.json");
    }

    if (const char* xdg_config = std::getenv("XDG_CONFIG_HOME"); xdg_config && *xdg_config) {
        candidates.emplace_back(std::filesystem::path(xdg_config) / "webradio" / "stations.json");
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        candidates.emplace_back(std::filesystem::path(home) / ".config" / "webradio" / "stations.json");
    }

    for (const auto& path : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec)) {
            return path.string();
        }
    }

    return "stations.json";
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

		g_pending_metadata = StreamMetadata{};
		g_pending_metadata.pending = true;

        g_pending_playing_state = false;
        g_has_playing_state_update = true;
    }
    
    bool is_playing() const {
        return g_playing;
    }

private:
	void update_metadata_tui(AVFormatContext * fmt_ctx, int audio_stream_idx)
	{
		auto check_metadata = [&](const char* key) -> AVDictionaryEntry*
		{
			AVDictionaryEntry* t = nullptr;
			if (audio_stream_idx >= 0 && fmt_ctx->streams[audio_stream_idx]) {
				t = av_dict_get(fmt_ctx->streams[audio_stream_idx]->metadata, key, nullptr, 0);
			}
			if (!t) {
				t = av_dict_get(fmt_ctx->metadata, key, nullptr, 0);
			}
			return t;
		};

		// Extract stream title (e.g., "a-ha - The Sun Always Shines on T.V.")
		if (AVDictionaryEntry* tag = check_metadata("StreamTitle"); tag && tag->value)
		{
			if (g_pending_metadata.title != tag->value)
			{
				g_pending_metadata.title = tag->value;
				g_pending_metadata.pending = true; // !g_pending_metadata.title.empty();
			}
		}

		// Extract genre information
		if (AVDictionaryEntry* tag = check_metadata("cy-genre"); tag && tag->value)
		{
			if (g_pending_metadata.genre != tag->value)
			{
				g_pending_metadata.genre = tag->value;
				g_pending_metadata.pending = true; // !g_pending_metadata.genre.empty();
			}
		}

		//AVDictionaryEntry* artist_tag = check_metadata("artist");
		//AVDictionaryEntry* title_tag = check_metadata("title");
	}


    bool play_stream(const std::string& url)
	{
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
        
        {
            std::string format_info = codec->name;
            if (!format_info.empty()) {
				format_info[0] = std::toupper(format_info[0]);
            }
            
            int bitrate_kbps = codec_ctx->bit_rate / 1000;
			if (bitrate_kbps > 0) {
				format_info = format_info + " " + std::to_string(bitrate_kbps) + "kbps";
			}

			g_tui->set_stream_format(format_info);
			g_tui->update_stream_kbps(bitrate_kbps);

        }
        
        constexpr int OUTPUT_SAMPLE_RATE = 44100;
        constexpr int OUTPUT_CHANNELS = 2;
        constexpr size_t OUTPUT_BYTES_PER_FRAME = 4;

        bool passthrough_pcm =
            codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16 &&
            codec_ctx->sample_rate == OUTPUT_SAMPLE_RATE &&
            codec_ctx->ch_layout.nb_channels == OUTPUT_CHANNELS;

        SwrContext* swr_ctx = nullptr;
        if (!passthrough_pcm) {
            swr_ctx = swr_alloc();
            if (!swr_ctx) {
                avcodec_free_context(&codec_ctx);
                avformat_close_input(&fmt_ctx);
                return false;
            }

            AVChannelLayout out_ch_layout;
            av_channel_layout_default(&out_ch_layout, OUTPUT_CHANNELS);

            ret = swr_alloc_set_opts2(&swr_ctx,
                &out_ch_layout,
                AV_SAMPLE_FMT_S16,
                OUTPUT_SAMPLE_RATE,
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
        }
        
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format = ma_format_s16;
        deviceConfig.playback.channels = OUTPUT_CHANNELS;
        deviceConfig.sampleRate = OUTPUT_SAMPLE_RATE;
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
        
        audio_buffer_.consumer_clear();
        constexpr size_t PREBUFFER_TARGET = 65536;

        auto write_to_audio_buffer = [&](const uint8_t* src, size_t data_size) {
            size_t written = 0;
            while (written < data_size && !stop_requested_) {
                uint8_t* dst = nullptr;
                size_t available = audio_buffer_.reserve_write_contiguous(dst);
                if (available == 0 || dst == nullptr) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                size_t chunk = std::min(available, data_size - written);
                std::memcpy(dst, src + written, chunk);
                audio_buffer_.produce(chunk);
                written += chunk;
            }
        };

        auto convert_frame_to_audio_buffer = [&](AVFrame* decoded_frame) {
            const uint8_t* const* input = const_cast<const uint8_t* const*>(decoded_frame->extended_data);
            int input_samples = decoded_frame->nb_samples;
            bool input_sent = false;

            while (!stop_requested_) {
                uint8_t* dst = nullptr;
                size_t available = audio_buffer_.reserve_write_contiguous(dst);
                if (available < OUTPUT_BYTES_PER_FRAME || dst == nullptr) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                int max_samples = static_cast<int>(available / OUTPUT_BYTES_PER_FRAME);
                int converted_samples = swr_convert(
                    swr_ctx,
                    &dst,
                    max_samples,
                    input_sent ? nullptr : input,
                    input_sent ? 0 : input_samples);

                if (converted_samples <= 0) {
                    return;
                }

                size_t produced_bytes = static_cast<size_t>(converted_samples) * OUTPUT_BYTES_PER_FRAME;
                audio_buffer_.produce(produced_bytes);
                input_sent = true;
            }
        };
        
        while (!stop_requested_ && audio_buffer_.read_available() < PREBUFFER_TARGET) {
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
                    
                    if (passthrough_pcm) {
                        int data_size = av_samples_get_buffer_size(
                            nullptr,
                            codec_ctx->ch_layout.nb_channels,
                            frame->nb_samples,
                            codec_ctx->sample_fmt,
                            1);
                        if (data_size > 0 && frame->data[0] != nullptr) {
                            write_to_audio_buffer(frame->data[0], static_cast<size_t>(data_size));
                        }
                    } else {
                        convert_frame_to_audio_buffer(frame);
                    }
                }
            }
            av_packet_unref(packet);
            
            size_t filled = audio_buffer_.read_available();
            int percent = static_cast<int>((filled * 100) / ByteRingbuffer::BUFFER_SIZE);
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


		update_metadata_tui(fmt_ctx, audio_stream_idx);

        
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
        
		g_bytes_accumulated = 0;
		g_last_kbps_calc = std::chrono::steady_clock::now();
		auto last_buffer_update = std::chrono::steady_clock::now();
		int old_buffer_percent = 0;
        while (!stop_requested_)
		{
            ret = av_read_frame(fmt_ctx, packet);
            if (ret < 0) break;
            
            g_bytes_accumulated += packet->size;
            
            if (packet->stream_index == audio_stream_idx)
			{
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret < 0) {
                    av_packet_unref(packet);
                    continue;
                }
                
                while (ret >= 0) {
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;
                    
                    if (passthrough_pcm) {
                        int data_size = av_samples_get_buffer_size(
                            nullptr,
                            codec_ctx->ch_layout.nb_channels,
                            frame->nb_samples,
                            codec_ctx->sample_fmt,
                            1);
                        if (data_size > 0 && frame->data[0] != nullptr) {
                            write_to_audio_buffer(frame->data[0], static_cast<size_t>(data_size));
                        }
                    } else {
                        convert_frame_to_audio_buffer(frame);
                    }
				}

				auto now_buffer = std::chrono::steady_clock::now();
				auto elapsed_buffer = std::chrono::duration_cast<std::chrono::milliseconds>(now_buffer - last_buffer_update).count();
				if (elapsed_buffer >= 1000)
				{
					update_metadata_tui(fmt_ctx, audio_stream_idx);


					size_t filled = audio_buffer_.read_available();
					int percent = static_cast<int>((filled * 100) / ByteRingbuffer::BUFFER_SIZE);
					if (percent > 100) percent = 100;
					if (old_buffer_percent != percent)
					{
						g_pending_buffer_percent = percent;
						old_buffer_percent = percent;
					}
					last_buffer_update = now_buffer;
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

        audio_buffer_.consumer_clear();
        return true;
    }
};

int main(int argc, char* argv[]) {
#ifndef FFMPEG_DEBUG_LOGGING
    av_log_set_callback(suppress_ffmpeg_logging);
#endif

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--version" || arg == "-v") {
            std::cout << WEBRADIO_VERSION << std::endl;
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::string stations_file = resolve_default_stations_file();
    if (argc > 1) {
        stations_file = argv[1];
    }

	std::vector<Station> stations = load_stations(stations_file);

#ifndef NDEBUG
	if (stations.empty()) {
		stations = {
			{"TRANCE","https://content.audioaddict.com/prd/9/a/7/1/9/352c5fe756c019b0988545caf517fbbb11f.mp4?purpose=playback&audio_token=9afc90f92811fba385d6aedd2c559352&network=di&device=chrome_145_windows_10&ip=155.4.125.79&ip_type=4&country_code=SE&show_id=13896&exp=2026-03-01T21:54:14Z&auth=6135aef9dae8e7277bd739a38c3a96bcd37292fb"},
			{"Bandit Rock", "https://fm02-ice.stream.khz.se/fm02_mp3?platform=web&aw_0_1st.playerid=mtgradio-web&aw_0_1st.skey=1770486477"},
			{"STAR FM", "https://fm05-ice.stream.khz.se/fm05_mp3?platform=web&aw_0_1st.playerid=mtgradio-web&aw_0_1st.skey=1770487025"},
			{"RIX FM", "https://fm01-ice.stream.khz.se/fm01_mp3?platform=web&aw_0_1st.playerid=mtgradio-web&aw_0_1st.skey=1770487082"},
			{"Svenska Favoriter", "https://fm06-ice.stream.khz.se/fm06_mp3?platform=web&aw_0_1st.playerid=mtgradio-web&aw_0_1st.skey=1770487119"},
			{"Rock Klassiker", "https://live-bauerse-fm.sharp-stream.com/rockklassiker_instream_se_aacp?direct=true&aw_0_1st.playerid=BMUK_inpage_html5&aw_0_1st.skey=1770662685"},
			{"MIX Megapol", "https://live-bauerse-fm.sharp-stream.com/mixmegapol_instream_se_aacp?direct=true&aw_0_1st.playerid=BMUK_inpage_html5&aw_0_1st.skey=1770662914"},
			{"Radio 45", "https://streaming.943.se/radio45"},
			{"Svensk POP", "https://live-bauerse-fm.sharp-stream.com/svenskpop_se_aacp?direct=true&aw_0_1st.playerid=BMUK_inpage_html5&aw_0_1st.skey=1770663244"},
			{"HYPR DemoScene", "https://hypr.website/hypr.mp3"}
		};
	}
#endif


    if (stations.empty()) {
        std::cerr << "No stations loaded" << std::endl;
        return 1;
    }
    
    g_tui = std::make_unique<RadioTUI>();
    if (!g_tui->init()) {
        std::cerr << "Failed to initialize TUI" << std::endl;
        return 1;
    }
    
    g_fft_spectrum = std::make_unique<FFTSpectrum>();
    
    g_tui->set_stations(stations);
    
    AudioPlayer player;
    
    g_tui->set_on_station_select([&player](const Station& station) {
        if (g_tui) {
            g_tui->set_song_title("","");
			g_pending_buffer_percent = 0;
        }
        player.play(station.url, station.name);
    });
    
    g_tui->set_on_stop([&player]() {
        player.stop();
        if (g_tui) {
            g_tui->set_song_title("","");
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
    
    g_tui->draw_all();
    
	while (g_running)
	{
		bool update_tui = false;
		bool only_spectrum_update = false;
        int ch = g_tui->get_input();
        if (ch != ERR) {
            g_tui->handle_input(ch);
        }
        
        {
            if (g_has_playing_state_update) {
				g_tui->set_current_station(g_current_station_name);
                g_tui->set_playing(g_pending_playing_state);
                g_has_playing_state_update = false;
            }
            
            int buffer_percent = g_pending_buffer_percent.load();
            if (buffer_percent >= 0 && g_tui) {
                g_tui->update_cache_info(buffer_percent);
                g_pending_buffer_percent = -1;
				update_tui = true;
            }
            
            
            if (g_pending_metadata.pending)
			{
				{
					std::lock_guard<std::mutex> lock(g_metadata_mutex);
					g_tui->set_song_title(g_pending_metadata.title, g_pending_metadata.genre);
					if (!g_pending_metadata.title.empty()) {
						g_tui->add_to_history(g_pending_metadata.title, g_current_station_name);
					}
					g_pending_metadata.pending = false;
				}
				update_tui = true;
			}
            
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_kbps_calc).count();
			if (elapsed >= 1000)
			{
				int kbps = static_cast<int>((g_bytes_accumulated * 1000) / (elapsed * 1024));
				g_tui->update_stream_kbps(kbps);
				g_bytes_accumulated = 0;
				g_last_kbps_calc = now;
				update_tui = true;
			}

		
			if(g_fft_spectrum)
			{
				g_fft_spectrum->process_samples();

	            if (g_fft_spectrum->has_new_data()) {
		            std::array<float, FFTSpectrum::NUM_BARS> spectrum_bars;
			        bool updated = false;
				    g_fft_spectrum->get_spectrum(spectrum_bars, updated);
					if (updated) {
						g_tui->update_spectrum(spectrum_bars);
						if (!update_tui) {
							only_spectrum_update = true;
						}
						update_tui = true;
					}
				}
			}

			if (update_tui) {
				if (only_spectrum_update) {
					g_tui->draw_spectrum_overlay();
				} else {
					g_tui->draw_main();
				}
			}
       }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    player.stop();
    
    g_tui->cleanup();
    
    return 0;
}
