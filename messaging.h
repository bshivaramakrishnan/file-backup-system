#pragma once

#include "common/types.h"
#include "common/logger.h"
#include "storage/database.h"
#include "datastructures/circular_buffer.h"

#include <string>
#include <vector>

namespace ecpb {

class MessagingService {
public:
    explicit MessagingService(Database& db) : db_(db), event_log_(256) {}

    // Create a channel
    int create_channel(const std::string& name) {
        int id = db_.create_channel(name);
        if (id >= 0) {
            log_event("Channel created: " + name);
        }
        return id;
    }

    // Send a text message
    bool send_message(const std::string& channel, const std::string& sender,
                      const std::string& content) {
        bool ok = db_.send_message(channel, sender, content, "text");
        if (ok) log_event(sender + " -> " + channel + ": " + content.substr(0, 50));
        return ok;
    }

    // Send a file-share notification
    bool share_file(const std::string& channel, const std::string& sender,
                    const std::string& file_path, int job_id) {
        std::string content = "[FILE] " + file_path + " (backup job: " + std::to_string(job_id) + ")";
        bool ok = db_.send_message(channel, sender, content, "file");
        if (ok) log_event(sender + " shared file in " + channel);
        return ok;
    }

    // Get recent messages
    std::vector<Database::Message> get_messages(const std::string& channel, int limit = 50) {
        return db_.get_messages(channel, limit);
    }

    // Get recent events
    std::vector<std::string> get_recent_events(size_t count = 20) {
        return event_log_.last_n(count);
    }

private:
    Database& db_;
    CircularBuffer<std::string> event_log_;

    void log_event(const std::string& event) {
        event_log_.push_overwrite(epoch_to_string(now_epoch_ms()) + " " + event);
    }
};

} // namespace ecpb
