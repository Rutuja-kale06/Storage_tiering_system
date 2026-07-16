#include "api/s3_server.hpp"
#include "api/s3_types.hpp"
#include "core/types.hpp"
#include "logger.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <iomanip>
#include <regex>
#include <algorithm>
#include <random>
#include <set>
#include <windows.h>
#include <wincrypt.h>

namespace fs = std::filesystem;
using namespace httplib;

S3Server::S3Server(std::shared_ptr<CatalogInterface> catalog,
                   const std::string& s3_base_path,
                   const std::string& api_key)
    : catalog_(std::move(catalog))
    , s3_base_path_(s3_base_path)
    , api_key_(api_key)
{}

bool S3Server::start(int port) {
    if (running_.load()) return false;
    port_ = port;
    running_.store(true);
    server_thread_ = std::thread([this, port]() { server_loop(port); });
    server_thread_.detach();
    LOG_INFO("S3 API", "Server starting on port " + std::to_string(port));
    return true;
}

void S3Server::stop() { running_.store(false); }

bool S3Server::verify_auth(const std::string& auth_header) {
    if (api_key_.empty()) return true;
    return auth_header == api_key_;
}

std::string S3Server::bucket_path(const std::string& bucket) const {
    return (fs::path(s3_base_path_) / bucket).string();
}

std::string S3Server::object_path(const std::string& bucket, const std::string& key) const {
    return (fs::path(s3_base_path_) / bucket / key).string();
}

std::string S3Server::multipart_upload_path(const std::string& bucket,
                                              const std::string& key,
                                              const std::string& upload_id) const {
    return (fs::path(s3_base_path_) / bucket / ".uploads" / upload_id / key).string();
}

std::string S3Server::generate_upload_id() const {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(gen) << dist(gen);
    return oss.str();
}

std::string S3Server::compute_md5(const std::string& data) const {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE md5[16];
    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return "";
    if (!CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }
    CryptHashData(hHash, reinterpret_cast<const BYTE*>(data.data()),
                  static_cast<DWORD>(data.size()), 0);
    DWORD md5len = 16;
    CryptGetHashParam(hHash, HP_HASHVAL, md5, &md5len, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    std::ostringstream oss;
    for (int i = 0; i < 16; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(md5[i]);
    return oss.str();
}

std::string S3Server::compute_file_md5(const std::string& filepath) const {
    std::ifstream f(filepath, std::ios::binary);
    if (!f) return "";
    std::ostringstream buf;
    buf << f.rdbuf();
    return compute_md5(buf.str());
}

// ── XML Builders ──────────────────────────────────────

std::string S3Server::build_error_xml(const std::string& code,
                                       const std::string& message,
                                       const std::string& resource) const {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<Error>"
        << "<Code>" << code << "</Code>"
        << "<Message>" << message << "</Message>";
    if (!resource.empty()) xml << "<Resource>" << resource << "</Resource>";
    xml << "</Error>";
    return xml.str();
}

std::string S3Server::build_list_buckets_xml(const std::vector<S3BucketInfo>& buckets) const {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<ListAllMyBucketsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        << "<Owner><ID>storage-tiering</ID><DisplayName>StorageTiering</DisplayName></Owner>"
        << "<Buckets>";
    for (const auto& b : buckets) {
        char timebuf[64];
        struct tm* tm_info = std::gmtime(&b.creation_date);
        std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
        xml << "<Bucket>"
            << "<Name>" << b.name << "</Name>"
            << "<CreationDate>" << timebuf << "</CreationDate>"
            << "</Bucket>";
    }
    xml << "</Buckets></ListAllMyBucketsResult>";
    return xml.str();
}

std::string S3Server::build_list_objects_xml(
    const std::vector<FileRecord>& files,
    const std::string& bucket,
    const std::string& prefix,
    const std::string& delimiter,
    int max_keys,
    const std::string& start_after,
    const std::vector<std::string>& common_prefixes,
    bool is_truncated) const
{
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        << "<Name>" << bucket << "</Name>"
        << "<Prefix>" << prefix << "</Prefix>";
    if (!delimiter.empty())
        xml << "<Delimiter>" << delimiter << "</Delimiter>";
    xml << "<MaxKeys>" << max_keys << "</MaxKeys>";
    if (!start_after.empty())
        xml << "<StartAfter>" << start_after << "</StartAfter>";
    xml << "<IsTruncated>" << (is_truncated ? "true" : "false") << "</IsTruncated>"
        << "<KeyCount>" << (files.size() + common_prefixes.size()) << "</KeyCount>";
    for (const auto& cp : common_prefixes)
        xml << "<CommonPrefixes><Prefix>" << cp << "</Prefix></CommonPrefixes>";
    for (const auto& f : files) {
        char timebuf[64];
        struct tm* tm_info = std::gmtime(&f.last_modified);
        std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
        xml << "<Contents>"
            << "<Key>" << f.s3_key << "</Key>"
            << "<LastModified>" << timebuf << "</LastModified>"
            << "<ETag>&quot;" << f.etag << "&quot;</ETag>"
            << "<Size>" << f.size_bytes << "</Size>"
            << "<StorageClass>" << TIER_NAMES[static_cast<int>(f.current_tier)] << "</StorageClass>"
            << "</Contents>";
    }
    xml << "</ListBucketResult>";
    return xml.str();
}

std::vector<S3PartInfo> S3Server::parse_complete_multipart_xml(const std::string& body) const {
    std::vector<S3PartInfo> parts;
    std::regex part_re("<Part>\\s*<PartNumber>(\\d+)</PartNumber>\\s*<ETag>\"?([^<\"]*)\"?</ETag>\\s*</Part>");
    auto words_begin = std::sregex_iterator(body.begin(), body.end(), part_re);
    auto words_end = std::sregex_iterator();
    for (auto it = words_begin; it != words_end; ++it) {
        S3PartInfo p;
        p.part_number = std::stoi((*it)[1].str());
        p.etag = (*it)[2].str();
        parts.push_back(p);
    }
    return parts;
}

// ── Helpers ───────────────────────────────────────────

static std::string detect_content_type(const std::string& key, const std::string& header_ct) {
    if (!header_ct.empty()) return header_ct;
    auto ext = fs::path(key).extension().string();
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".js") return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".txt") return "text/plain";
    if (ext == ".xml") return "application/xml";
    if (ext == ".zip") return "application/zip";
    return "application/octet-stream";
}

static Tier storage_class_to_tier(const std::string& sc) {
    if (sc == "STANDARD_IA" || sc == "WARM") return Tier::WARM;
    if (sc == "GLACIER" || sc == "COLD") return Tier::COLD;
    if (sc == "DEEP_ARCHIVE" || sc == "ARCHIVE") return Tier::ARCHIVE;
    return Tier::HOT; // STANDARD or anything else
}

static std::string fmt_gmtime(time_t t) {
    char buf[64];
    struct tm* tm_info = std::gmtime(&t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
    return std::string(buf);
}

// ── Server Loop ───────────────────────────────────────

void S3Server::server_loop(int port) {
    Server svr;

    svr.set_default_headers({
        {"Server", "StorageTieringS3/1.0"},
        {"x-amz-request-id", "storage-tiering-s3"}
    });

    auto auth = [&](const Request& req, Response& res) -> bool {
        if (api_key_.empty()) return true;
        std::string key = req.get_header_value("x-amx-api-key");
        if (key.empty()) key = req.get_param_value("api_key");
        if (key != api_key_) {
            res.status = 403;
            res.set_content(build_error_xml("AccessDenied", "Access denied"), "application/xml");
            return false;
        }
        return true;
    };

    // ── GET /  List All Buckets ──
    svr.Get("/", [&](const Request& req, Response& res) {
        if (!auth(req, res)) return;
        std::vector<S3BucketInfo> buckets;
        if (fs::exists(s3_base_path_)) {
            for (const auto& entry : fs::directory_iterator(s3_base_path_)) {
                if (!entry.is_directory()) continue;
                std::string name = entry.path().filename().string();
                if (name[0] == '.') continue;
                S3BucketInfo b;
                b.name = name;
                auto ftime = entry.last_write_time();
                auto sctime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                b.creation_date = std::chrono::system_clock::to_time_t(sctime);
                buckets.push_back(b);
            }
        }
        res.set_content(build_list_buckets_xml(buckets), "application/xml");
    });

    // ── PUT /{bucket}  CreateBucket ──
    svr.Put(R"(/([^/]+)$)", [&](const Request& req, Response& res) {
        if (!auth(req, res)) return;
        std::string bucket = req.matches[1];
        fs::path bp = fs::path(s3_base_path_) / bucket;
        if (fs::exists(bp)) {
            res.status = 409;
            res.set_content(build_error_xml("BucketAlreadyExists",
                           "The requested bucket already exists"), "application/xml");
            return;
        }
        std::error_code ec;
        fs::create_directories(bp, ec);
        if (ec) {
            res.status = 500;
            res.set_content(build_error_xml("InternalError", ec.message()), "application/xml");
            return;
        }
        res.status = 200;
    });

    // ── HEAD /{bucket}  HeadBucket (handled via Get with method check) ──
    // (HEAD requests handled automatically by cpp-httplib for GET routes)

    // ── DELETE /{bucket}  DeleteBucket ──
    svr.Delete(R"(/([^/]+)$)", [&](const Request& req, Response& res) {
        if (!auth(req, res)) return;
        std::string bucket = req.matches[1];
        fs::path bp = fs::path(s3_base_path_) / bucket;
        if (!fs::exists(bp)) {
            res.status = 404;
            res.set_content(build_error_xml("NoSuchBucket",
                           "The specified bucket does not exist"), "application/xml");
            return;
        }
        bool non_empty = false;
        for (auto it = fs::recursive_directory_iterator(bp, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file()) { non_empty = true; break; }
        }
        if (non_empty) {
            res.status = 409;
            res.set_content(build_error_xml("BucketNotEmpty",
                           "The bucket you tried to delete is not empty"), "application/xml");
            return;
        }
        std::error_code ec;
        fs::remove(bp, ec);
        if (ec) {
            res.status = 500;
            res.set_content(build_error_xml("InternalError", ec.message()), "application/xml");
            return;
        }
        auto all = catalog_->all_files();
        for (auto& f : all)
            if (f.s3_bucket == bucket) catalog_->delete_file(f.id);
        res.status = 204;
    });

    // ── GET /{bucket}  List Objects ──
    svr.Get(R"(/([^/]+)$)", [&](const Request& req, Response& res) {
        if (!auth(req, res)) return;
        std::string bucket = req.matches[1];
        fs::path bp = fs::path(s3_base_path_) / bucket;
        if (!fs::exists(bp)) {
            res.status = 404;
            res.set_content(build_error_xml("NoSuchBucket",
                           "The specified bucket does not exist"), "application/xml");
            return;
        }
        std::string prefix = req.get_param_value("prefix", false);
        std::string delimiter = req.get_param_value("delimiter", false);
        std::string start_after = req.get_param_value("start-after", false);
        std::string cont_token = req.get_param_value("continuation-token", false);
        int max_keys = 1000;
        if (req.has_param("max-keys")) {
            try { max_keys = std::min(1000, std::max(1, std::stoi(req.get_param_value("max-keys", false)))); }
            catch (...) {}
        }
        auto all = catalog_->all_files();
        std::vector<FileRecord> matching;
        std::set<std::string> seen_prefixes;
        for (auto& f : all) {
            if (f.s3_bucket != bucket) continue;
            if (!prefix.empty() && f.s3_key.find(prefix) != 0) continue;
            std::string cmp = start_after.empty() ? cont_token : start_after;
            if (!cmp.empty() && f.s3_key <= cmp) continue;
            if (!delimiter.empty()) {
                std::string remaining = f.s3_key.substr(prefix.length());
                auto pos = remaining.find(delimiter);
                if (pos != std::string::npos) {
                    seen_prefixes.insert(prefix + remaining.substr(0, pos + delimiter.length()));
                    continue;
                }
            }
            matching.push_back(f);
            if (static_cast<int>(matching.size()) >= max_keys) break;
        }
        bool is_truncated = static_cast<int>(matching.size()) >= max_keys;
        std::vector<std::string> common_prefixes(seen_prefixes.begin(), seen_prefixes.end());
        res.set_content(
            build_list_objects_xml(matching, bucket, prefix, delimiter,
                                   max_keys, start_after, common_prefixes, is_truncated),
            "application/xml");
    });

    // ── PUT /{bucket}/{key+}  PutObject / UploadPart ──
    svr.Put(R"(/([^/]+)/(.+))", [&](const Request& req, Response& res) {
        if (!auth(req, res)) return;
        std::string bucket = req.matches[1];
        std::string key = req.matches[2];

        // Check for multipart upload part
        if (req.has_param("uploadId") && req.has_param("partNumber")) {
            // ── Upload Part ──
            std::string upload_id = req.get_param_value("uploadId");
            int part_number = std::stoi(req.get_param_value("partNumber"));
            fs::path part_dir = fs::path(s3_base_path_) / bucket / ".uploads" / upload_id;
            if (!fs::exists(part_dir)) {
                res.status = 404;
                res.set_content(build_error_xml("NoSuchUpload",
                               "The specified upload does not exist"), "application/xml");
                return;
            }
            fs::path part_path = part_dir / ("part." + std::to_string(part_number));
            {
                std::ofstream ofs(part_path, std::ios::binary);
                if (!ofs) {
                    res.status = 500;
                    res.set_content(build_error_xml("InternalError", "Failed to write part"),
                                   "application/xml");
                    return;
                }
                ofs.write(req.body.data(), req.body.size());
            }
            std::string etag = compute_md5(req.body);
            res.status = 200;
            res.set_header("ETag", "\"" + etag + "\"");
            return;
        }

        // ── Put Object ──
        fs::path bp = fs::path(s3_base_path_) / bucket;
        if (!fs::exists(bp)) {
            std::error_code ec;
            fs::create_directories(bp, ec);
            if (ec) {
                res.status = 500;
                res.set_content(build_error_xml("InternalError", ec.message()), "application/xml");
                return;
            }
        }
        std::string etag = compute_md5(req.body);
        fs::path op = fs::path(s3_base_path_) / bucket / key;
        std::error_code ec;
        fs::create_directories(op.parent_path(), ec);
        {
            std::ofstream ofs(op, std::ios::binary);
            if (!ofs) {
                res.status = 500;
                res.set_content(build_error_xml("InternalError", "Failed to write file"),
                               "application/xml");
                return;
            }
            ofs.write(req.body.data(), req.body.size());
        }
        std::string content_type = detect_content_type(key, req.get_header_value("Content-Type"));
        Tier tier = storage_class_to_tier(req.get_header_value("x-amz-storage-class"));
        std::string file_id = bucket + "/" + key;
        auto existing = catalog_->get_file(file_id);
        FileRecord f;
        f.id = file_id;
        f.path = op.string();
        f.extension = fs::path(key).extension().string();
        f.file_type = classify_extension(f.extension);
        f.current_tier = tier;
        f.target_tier = tier;
        f.size_bytes = static_cast<int64_t>(req.body.size());
        f.access_count = 0;
        f.write_count = 0;
        f.created_at = existing ? existing->created_at : std::time(nullptr);
        f.last_accessed = std::time(nullptr);
        f.last_modified = std::time(nullptr);
        f.migrate_count = existing ? existing->migrate_count : 0;
        f.is_pinned = false;
        f.is_critical = false;
        f.score = 0.0;
        f.s3_bucket = bucket;
        f.s3_key = key;
        f.content_type = content_type;
        f.etag = etag;
        if (existing) catalog_->update_file(f);
        else catalog_->add_file(f);
        res.status = 200;
        res.set_header("ETag", "\"" + etag + "\"");
    });

    // ── GET /{bucket}/{key+}  GetObject ──
    svr.Get(R"(/([^/]+)/(.+))", [&](const Request& req, Response& res) {
        if (!auth(req, res)) return;
        std::string bucket = req.matches[1];
        std::string key = req.matches[2];
        std::string file_id = bucket + "/" + key;
        auto existing = catalog_->get_file(file_id);
        if (!existing || !fs::exists(existing->path)) {
            res.status = 404;
            res.set_content(build_error_xml("NoSuchKey",
                           "The specified key does not exist"), "application/xml");
            return;
        }
        catalog_->record_access(file_id);
        std::ifstream ifs(existing->path, std::ios::binary | std::ios::ate);
        if (!ifs) {
            res.status = 500;
            res.set_content(build_error_xml("InternalError", "Failed to read file"),
                           "application/xml");
            return;
        }
        auto size = ifs.tellg();
        ifs.seekg(0);
        std::string content((std::istreambuf_iterator<char>(ifs)), {});
        std::string ct = existing->content_type.empty() ? "application/octet-stream" : existing->content_type;
        res.set_header("Content-Type", ct);
        res.set_header("ETag", "\"" + existing->etag + "\"");
        res.set_header("Content-Length", std::to_string(size));
        res.set_header("Last-Modified", fmt_gmtime(existing->last_modified));
        res.set_header("x-amz-storage-class", TIER_NAMES[static_cast<int>(existing->current_tier)]);
        res.set_content(content, ct);
    });

    // ── HEAD /{bucket}/{key+}  HeadObject (handled via Get with method check) ──
    // (HEAD requests handled automatically by cpp-httplib for GET routes)

    // ── DELETE /{bucket}/{key+}  DeleteObject / AbortMultipartUpload ──
    svr.Delete(R"(/([^/]+)/(.+))", [&](const Request& req, Response& res) {
        if (!auth(req, res)) return;
        std::string bucket = req.matches[1];
        std::string key = req.matches[2];

        if (req.has_param("uploadId")) {
            // ── Abort Multipart Upload ──
            std::string upload_id = req.get_param_value("uploadId");
            fs::path upload_dir = fs::path(s3_base_path_) / bucket / ".uploads" / upload_id;
            if (fs::exists(upload_dir)) {
                std::error_code ec;
                fs::remove_all(upload_dir, ec);
            }
            res.status = 204;
            return;
        }

        // ── Delete Object ──
        std::string file_id = bucket + "/" + key;
        auto existing = catalog_->get_file(file_id);
        if (existing) {
            std::error_code ec;
            fs::remove(existing->path, ec);
            catalog_->delete_file(file_id);
        }
        res.status = 204;
    });

    // ── POST /{bucket}/{key+}  InitiateMultipartUpload / CompleteMultipartUpload ──
    svr.Post(R"(/([^/]+)/(.+))", [&](const Request& req, Response& res) {
        if (!auth(req, res)) return;
        std::string bucket = req.matches[1];
        std::string key = req.matches[2];

        // Check for InitiateMultipartUpload: ?uploads
        if (req.has_param("uploads")) {
            // ── Initiate Multipart Upload ──
            std::string upload_id = generate_upload_id();
            fs::path upload_dir = fs::path(s3_base_path_) / bucket / ".uploads" / upload_id;
            std::error_code ec;
            fs::create_directories(upload_dir, ec);
            nlohmann::json meta;
            meta["upload_id"] = upload_id;
            meta["bucket"] = bucket;
            meta["key"] = key;
            meta["initiated"] = std::time(nullptr);
            std::ofstream mf(upload_dir / ".metadata.json");
            if (mf) mf << meta.dump();
            std::ostringstream xml;
            xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                << "<InitiateMultipartUploadResult>"
                << "<Bucket>" << bucket << "</Bucket>"
                << "<Key>" << key << "</Key>"
                << "<UploadId>" << upload_id << "</UploadId>"
                << "</InitiateMultipartUploadResult>";
            res.status = 200;
            res.set_content(xml.str(), "application/xml");
            return;
        }

        // Check for CompleteMultipartUpload: ?uploadId
        if (req.has_param("uploadId")) {
            // ── Complete Multipart Upload ──
            std::string upload_id = req.get_param_value("uploadId");
            fs::path upload_dir = fs::path(s3_base_path_) / bucket / ".uploads" / upload_id;
            if (!fs::exists(upload_dir)) {
                res.status = 404;
                res.set_content(build_error_xml("NoSuchUpload",
                               "The specified upload does not exist"), "application/xml");
                return;
            }
            std::vector<S3PartInfo> parts = parse_complete_multipart_xml(req.body);
            if (parts.empty()) {
                res.status = 400;
                res.set_content(build_error_xml("MalformedXML",
                               "The XML you provided was not well-formed"), "application/xml");
                return;
            }
            std::sort(parts.begin(), parts.end(),
                [](const S3PartInfo& a, const S3PartInfo& b) { return a.part_number < b.part_number; });
            fs::path output_path = fs::path(s3_base_path_) / bucket / key;
            fs::create_directories(output_path.parent_path());
            std::ofstream ofs(output_path, std::ios::binary);
            if (!ofs) {
                res.status = 500;
                res.set_content(build_error_xml("InternalError", "Failed to write output file"),
                               "application/xml");
                return;
            }
            std::string combined_md5_input;
            for (const auto& part : parts) {
                fs::path part_path = upload_dir / ("part." + std::to_string(part.part_number));
                if (!fs::exists(part_path)) {
                    res.status = 400;
                    res.set_content(build_error_xml("InvalidPart",
                                   "One or more specified parts could not be found"), "application/xml");
                    return;
                }
                std::ifstream ifs(part_path, std::ios::binary);
                std::string chunk((std::istreambuf_iterator<char>(ifs)), {});
                ifs.close();
                ofs.write(chunk.data(), chunk.size());
                combined_md5_input += part.etag;
            }
            ofs.close();
            std::string composite_md5 = compute_md5(combined_md5_input);
            std::string etag = composite_md5 + "-" + std::to_string(parts.size());
            auto total_size = fs::file_size(output_path);
            std::string file_id = bucket + "/" + key;
            auto existing = catalog_->get_file(file_id);
            FileRecord f;
            f.id = file_id;
            f.path = output_path.string();
            f.extension = fs::path(key).extension().string();
            f.file_type = classify_extension(f.extension);
            f.current_tier = Tier::HOT;
            f.target_tier = Tier::HOT;
            f.size_bytes = static_cast<int64_t>(total_size);
            f.access_count = 0;
            f.write_count = 0;
            f.created_at = existing ? existing->created_at : std::time(nullptr);
            f.last_accessed = std::time(nullptr);
            f.last_modified = std::time(nullptr);
            f.migrate_count = existing ? existing->migrate_count : 0;
            f.is_pinned = false;
            f.is_critical = false;
            f.score = 0.0;
            f.s3_bucket = bucket;
            f.s3_key = key;
            f.content_type = "application/octet-stream";
            f.etag = etag;
            if (existing) catalog_->update_file(f);
            else catalog_->add_file(f);
            std::error_code ec;
            fs::remove_all(upload_dir, ec);
            std::ostringstream xml;
            xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                << "<CompleteMultipartUploadResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                << "<Bucket>" << bucket << "</Bucket>"
                << "<Key>" << key << "</Key>"
                << "<ETag>&quot;" << etag << "&quot;</ETag>"
                << "</CompleteMultipartUploadResult>";
            res.status = 200;
            res.set_content(xml.str(), "application/xml");
            return;
        }

        res.status = 400;
        res.set_content(build_error_xml("BadRequest", "Unrecognized POST request"), "application/xml");
    });

    LOG_INFO("S3 API", "Listening on http://localhost:" + std::to_string(port));
    svr.listen("0.0.0.0", port);
}
