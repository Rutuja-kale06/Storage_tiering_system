#pragma once

#include "api/s3_types.hpp"
#include "catalog/catalog_interface.hpp"
#include "core/file_record.hpp"
#include <memory>
#include <string>
#include <thread>
#include <atomic>

class S3Server {
public:
    S3Server(std::shared_ptr<CatalogInterface> catalog,
             const std::string& s3_base_path,
             const std::string& api_key = "");

    bool start(int port = 3001);
    void stop();
    bool is_running() const { return running_.load(); }
    int  port() const { return port_; }

private:
    std::shared_ptr<CatalogInterface> catalog_;
    std::string s3_base_path_;
    std::string api_key_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    int port_ = 3001;

    void server_loop(int port);

    // Auth
    bool verify_auth(const std::string& auth_header);

    // Path helpers
    std::string bucket_path(const std::string& bucket) const;
    std::string object_path(const std::string& bucket, const std::string& key) const;

    // XML builders
    std::string build_error_xml(const std::string& code, const std::string& message,
                                const std::string& resource = "") const;
    std::string build_list_buckets_xml(const std::vector<S3BucketInfo>& buckets) const;
    std::string build_list_objects_xml(const std::vector<FileRecord>& files,
                                       const std::string& bucket,
                                       const std::string& prefix,
                                       const std::string& delimiter,
                                       int max_keys,
                                       const std::string& start_after,
                                       const std::vector<std::string>& common_prefixes,
                                       bool is_truncated) const;

    // MD5 helper
    std::string compute_md5(const std::string& data) const;
    std::string compute_file_md5(const std::string& filepath) const;

    // Multipart helpers
    std::string multipart_upload_path(const std::string& bucket,
                                       const std::string& key,
                                       const std::string& upload_id) const;
    std::string generate_upload_id() const;
    std::vector<S3PartInfo> parse_complete_multipart_xml(const std::string& body) const;
};
