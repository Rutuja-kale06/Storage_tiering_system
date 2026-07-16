#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct S3BucketInfo {
    std::string name;
    time_t      creation_date = 0;
};

struct S3ObjectInfo {
    std::string key;
    std::string etag;
    int64_t     size_bytes = 0;
    time_t      last_modified = 0;
    std::string storage_class;
    std::string content_type;
};

struct S3ListResult {
    std::vector<S3ObjectInfo> contents;
    std::vector<std::string>  common_prefixes;
    bool                      is_truncated = false;
    std::string               next_continuation_token;
    int                       key_count = 0;
    int                       max_keys = 1000;
    std::string               delimiter;
    std::string               prefix;
};

struct S3PartInfo {
    int         part_number = 0;
    std::string etag;
};

struct S3MultipartUpload {
    std::string upload_id;
    std::string bucket;
    std::string key;
    time_t      initiated = 0;
    std::vector<S3PartInfo> parts;
};
