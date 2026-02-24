#include "metadata_fetcher.hpp"

#ifdef ENABLE_MUSICBRAINZ

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace metadata {

// CURL write callback
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

MusicBrainzFetcher::MusicBrainzFetcher() {
    last_request_time_ = std::chrono::steady_clock::now() - MIN_REQUEST_INTERVAL;
}

MusicBrainzFetcher::~MusicBrainzFetcher() {
    stop();
}

void MusicBrainzFetcher::start() {
    if (!running_.exchange(true)) {
        worker_ = std::thread(&MusicBrainzFetcher::workerThread, this);
    }
}

void MusicBrainzFetcher::stop() {
    if (running_.exchange(false)) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_cv_.notify_all();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }
}

void MusicBrainzFetcher::request(const std::string& artist, const std::string& title) {
    if (artist.empty() && title.empty()) {
        return;
    }
    
    std::string key = makeCacheKey(artist, title);
    std::string query_str = artist + " - " + title;
    
    // Check if already in cache
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cache_.find(key) != cache_.end()) {
            return;  // Already cached
        }
        // Mark as "requested" by inserting empty entry
        cache_[key] = TrackInfo{};
    }
    
    // Report waiting status
    reportStatus("Waiting...", query_str);
    
    // Add to queue
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(FetchRequest{artist, title, key});
        queue_cv_.notify_one();
    }
}

bool MusicBrainzFetcher::hasResult(const std::string& artist, const std::string& title) const {
    std::string key = makeCacheKey(artist, title);
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second.available;
    }
    return false;
}

TrackInfo MusicBrainzFetcher::getResult(const std::string& artist, const std::string& title) {
    std::string key = makeCacheKey(artist, title);
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }
    return TrackInfo{};
}

void MusicBrainzFetcher::clearCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.clear();
}

void MusicBrainzFetcher::setStatusCallback(StatusCallback callback) {
    status_callback_ = callback;
}

void MusicBrainzFetcher::reportStatus(const std::string& status,
                                      const std::string& query,
                                      const std::string& album,
                                      const std::string& year,
                                      const std::string& genre,
                                      int score,
                                      bool has_result,
                                      const std::string& error_message) {
    if (status_callback_) {
        status_callback_(status, query, album, year, genre, score, has_result, error_message);
    }
}

void MusicBrainzFetcher::workerThread() {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return;
    }
    
    // Set common CURL options
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "radio-player/1.0 (radio-player@localhost)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, REQUEST_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    
    while (running_) {
        FetchRequest request;
        
        // Wait for request
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
            
            if (!running_) {
                break;
            }
            
            if (queue_.empty()) {
                continue;
            }
            
            request = queue_.front();
            queue_.pop();
        }
        
        // Rate limiting: ensure 1 second between requests
        {
            std::unique_lock<std::mutex> lock(rate_mutex_);
            auto now = std::chrono::steady_clock::now();
            auto elapsed = now - last_request_time_;
            if (elapsed < MIN_REQUEST_INTERVAL) {
                std::this_thread::sleep_for(MIN_REQUEST_INTERVAL - elapsed);
            }
            last_request_time_ = std::chrono::steady_clock::now();
        }
        
        // Report that we're querying
        std::string query_str = request.artist + " - " + request.title;
        reportStatus("Querying...", query_str);
        
        // Query API
        TrackInfo info = queryAPI(request.artist, request.title);
        
        // Report result
        if (info.available) {
            reportStatus("Received", query_str, info.album, info.year, info.genre, info.score, true);
        } else {
            reportStatus("Not found", query_str, "", "", "", 0, false);
        }
        
        // Store result
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_[request.key] = info;
        }
    }
    
    curl_easy_cleanup(curl);
}

TrackInfo MusicBrainzFetcher::queryAPI(const std::string& artist, const std::string& title) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return TrackInfo{};
    }
    
    // Build query URL
    std::string query;
    if (!artist.empty() && !title.empty()) {
        query = "recording:\"" + urlEncode(title) + "\"%20AND%20artist:\"" + urlEncode(artist) + "\"";
    } else if (!title.empty()) {
        query = "recording:\"" + urlEncode(title) + "\"";
    } else {
        query = "artist:\"" + urlEncode(artist) + "\"";
    }
    
    std::string url = "https://musicbrainz.org/ws/2/recording/?query=" + query + "&fmt=json&limit=1";
    
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "radio-player/1.0 (radio-player@localhost)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, REQUEST_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        return TrackInfo{};
    }
    
    return parseResponse(response);
}

// Helper to score a release (higher = better match for original album)
static int scoreRelease(const nlohmann::json& release) {
    int score = 0;
    
    // Check status
    if (release.contains("status")) {
        std::string status = release["status"].get<std::string>();
        if (status == "Official") score += 10;
        else if (status == "Bootleg") score -= 20;
        else score += 5;  // Promotion, Pseudo-Release
    } else {
        score += 5;  // Default
    }
    
    // Check primary type via release-group
    if (release.contains("release-group") && 
        release["release-group"].contains("primary-type")) {
        std::string type = release["release-group"]["primary-type"].get<std::string>();
        if (type == "Album") score += 10;
        else if (type == "EP") score += 5;
        else if (type == "Single") score += 3;
    }
    
    // Check secondary types (penalize bad ones heavily)
    if (release.contains("release-group") &&
        release["release-group"].contains("secondary-types")) {
        for (const auto& st : release["release-group"]["secondary-types"]) {
            std::string secondary = st.get<std::string>();
            if (secondary == "Compilation") score -= 100;
            else if (secondary == "Live") score -= 50;
            else if (secondary == "Remix") score -= 40;
            else if (secondary == "DJ-mix") score -= 30;
            else if (secondary == "Mixtape/Street") score -= 30;
            else if (secondary == "Spokenword") score -= 25;
            else if (secondary == "Interview") score -= 25;
            else if (secondary == "Audiobook") score -= 25;
            else if (secondary == "Audio drama") score -= 25;
            else if (secondary == "Soundtrack") score -= 20;
        }
    }
    
    return score;
}

TrackInfo MusicBrainzFetcher::parseResponse(const std::string& json_str) {
    TrackInfo info;
    
    try {
        auto json = nlohmann::json::parse(json_str);
        
        if (!json.contains("recordings") || json["recordings"].empty()) {
            return info;
        }
        
        const auto& recording = json["recordings"][0];
        
        // Find the best release (original studio album, not compilation)
        if (recording.contains("releases") && !recording["releases"].empty()) {
            const auto& releases = recording["releases"];
            
            int best_score = -1000;
            size_t best_idx = 0;
            std::string best_year;
            
            for (size_t i = 0; i < releases.size(); ++i) {
                const auto& release = releases[i];
                int score = scoreRelease(release);
                
                // Skip releases with very low scores (compilations, etc.)
                if (score < 0) {
                    continue;
                }
                
                // Extract year for this release
                std::string year;
                if (release.contains("date")) {
                    std::string date = release["date"].get<std::string>();
                    if (date.length() >= 4) {
                        year = date.substr(0, 4);
                    }
                }
                
                // Prefer earlier releases (original albums over reissues)
                if (!year.empty()) {
                    // Parse year as int, earlier = better
                    try {
                        int year_val = std::stoi(year);
                        // Bonus for being earlier (but don't penalize too much for unknown dates)
                        score += std::max(0, 2100 - year_val) / 10;  // Earlier albums get higher score
                    } catch (...) {
                        // Invalid year format, ignore
                    }
                }
                
                // Track the best one
                if (score > best_score) {
                    best_score = score;
                    best_idx = i;
                    best_year = year;
                } else if (score == best_score && !year.empty()) {
                    // Tie-breaker: prefer earlier date
                    try {
                        int curr_best_year = best_year.empty() ? 9999 : std::stoi(best_year);
                        int this_year = std::stoi(year);
                        if (this_year < curr_best_year) {
                            best_idx = i;
                            best_year = year;
                        }
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }
            
            // Use the best release we found
            if (best_score >= 0) {
                const auto& best_release = releases[best_idx];
                
                if (best_release.contains("title")) {
                    info.album = best_release["title"].get<std::string>();
                }
                
                info.year = best_year;
                info.score = best_score;  // Store the score for debugging
            }
        }
        
        // Get primary genre from tags
        if (recording.contains("tags") && !recording["tags"].empty()) {
            const auto& tags = recording["tags"];
            int max_count = -1;
            for (const auto& tag : tags) {
                if (tag.contains("name") && tag.contains("count")) {
                    int count = tag["count"].get<int>();
                    if (count > max_count) {
                        max_count = count;
                        info.genre = tag["name"].get<std::string>();
                    }
                }
            }
        }
        
        // Alternative: check artist tags if no recording tags
        if (info.genre.empty() && recording.contains("artist-credit") && 
            !recording["artist-credit"].empty()) {
            const auto& artist = recording["artist-credit"][0];
            if (artist.contains("artist") && artist["artist"].contains("tags")) {
                const auto& tags = artist["artist"]["tags"];
                int max_count = -1;
                for (const auto& tag : tags) {
                    if (tag.contains("name") && tag.contains("count")) {
                        int count = tag["count"].get<int>();
                        if (count > max_count) {
                            max_count = count;
                            info.genre = tag["name"].get<std::string>();
                        }
                    }
                }
            }
        }
        
        // Only mark as available if we found at least some data
        info.available = !info.album.empty() || !info.year.empty() || !info.genre.empty();
        
    } catch (const std::exception& e) {
        // Parse error, return empty info
    }
    
    return info;
}

std::string MusicBrainzFetcher::urlEncode(const std::string& str) {
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex;
    
    for (char c : str) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else {
            encoded << std::uppercase;
            encoded << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
            encoded << std::nouppercase;
        }
    }
    
    return encoded.str();
}

std::string MusicBrainzFetcher::makeCacheKey(const std::string& artist, const std::string& title) {
    return normalize(artist) + " - " + normalize(title);
}

std::string MusicBrainzFetcher::normalize(const std::string& str) {
    std::string result = str;
    // Convert to lowercase
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    // Trim leading/trailing whitespace
    size_t start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = result.find_last_not_of(" \t\n\r");
    return result.substr(start, end - start + 1);
}

} // namespace metadata

#endif // ENABLE_MUSICBRAINZ
