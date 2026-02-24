#ifndef METADATA_FETCHER_HPP
#define METADATA_FETCHER_HPP

#ifdef ENABLE_MUSICBRAINZ

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <optional>

namespace metadata {

// Track metadata information from MusicBrainz
struct TrackInfo {
    std::string album;
    std::string year;
    std::string genre;
    bool available = false;
    int score = 0;  // Selection score for debugging
    
    bool empty() const {
        return album.empty() && year.empty() && genre.empty();
    }
};

// Request structure for the fetch queue
struct FetchRequest {
    std::string artist;
    std::string title;
    std::string key;  // Cache key: "artist - title"
};

// Status callback for debugging
using StatusCallback = void(*)(const std::string& status,
                               const std::string& query,
                               const std::string& album,
                               const std::string& year,
                               const std::string& genre,
                               int score,
                               bool has_result,
                               const std::string& error_message);

// MusicBrainz API client with rate limiting
class MusicBrainzFetcher {
public:
    MusicBrainzFetcher();
    ~MusicBrainzFetcher();
    
    // Start the background fetcher thread
    void start();
    
    // Stop the background fetcher thread
    void stop();
    
    // Request metadata for a track (non-blocking)
    // Returns immediately, result available later via getResult()
    void request(const std::string& artist, const std::string& title);
    
    // Check if result is available for a track
    bool hasResult(const std::string& artist, const std::string& title) const;
    
    // Get result for a track (non-blocking, returns empty if not ready)
    TrackInfo getResult(const std::string& artist, const std::string& title);
    
    // Clear the cache (call when changing stations if desired)
    void clearCache();
    
    // Set status callback for debugging
    void setStatusCallback(StatusCallback callback);
    
private:
    // Helper to report status
    void reportStatus(const std::string& status,
                      const std::string& query = "",
                      const std::string& album = "",
                      const std::string& year = "",
                      const std::string& genre = "",
                      int score = 0,
                      bool has_result = false,
                      const std::string& error_message = "");
    // Background worker thread
    void workerThread();
    
    // Query MusicBrainz API
    TrackInfo queryAPI(const std::string& artist, const std::string& title);
    
    // Parse JSON response
    TrackInfo parseResponse(const std::string& json_str);
    
    // URL encoding for API query
    static std::string urlEncode(const std::string& str);
    
    // Create cache key from artist and title
    static std::string makeCacheKey(const std::string& artist, const std::string& title);
    
    // Normalize string for cache key (lowercase, trim)
    static std::string normalize(const std::string& str);
    
    // Worker thread
    std::thread worker_;
    
    // Request queue
    std::queue<FetchRequest> queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    // Result cache
    std::unordered_map<std::string, TrackInfo> cache_;
    mutable std::mutex cache_mutex_;
    
    // Control flag
    std::atomic<bool> running_{false};
    
    // Rate limiting
    std::chrono::steady_clock::time_point last_request_time_;
    mutable std::mutex rate_mutex_;
    
    // Status callback for debugging
    StatusCallback status_callback_ = nullptr;
    
    static constexpr auto MIN_REQUEST_INTERVAL = std::chrono::seconds(1);
    static constexpr auto REQUEST_TIMEOUT_MS = 5000;
};

} // namespace metadata

#endif // ENABLE_MUSICBRAINZ

#endif // METADATA_FETCHER_HPP
