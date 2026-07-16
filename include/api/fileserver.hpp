#pragma once

#include <string>

class FileServer {
public:
    explicit FileServer(const std::string& base_path);

    void set_user(const std::string& username, bool is_admin);

    // Returns JSON: { files: [{name, size, modified, is_dir}], path }
    std::string handle_list(const std::string& path_str, std::string& out_path) const;

    // Save uploaded data to path
    bool handle_upload(const std::string& path_str, const std::string& data) const;

    // Read file for download (returns false if not found)
    bool handle_download(const std::string& path_str, std::string& out_data, std::string& out_mime) const;

    bool handle_delete(const std::string& path_str) const;
    bool handle_mkdir(const std::string& path_str) const;
    bool handle_rename(const std::string& old_path_str, const std::string& new_path_str) const;

    // Returns JSON stats for the base path
    std::string handle_stats() const;

    // Returns the fileserver HTML content
    static std::string html_content();

    // Resolves a virtual path to a real filesystem path, ensuring no traversal
    // Returns false if path is unsafe
    bool resolve(const std::string& virtual_path, std::string& real_path) const;

private:
    std::string base_path_;
    std::string current_user_;
};
