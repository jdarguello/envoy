#pragma once

#include <memory>

#include "envoy/extensions/http/cache/file_system_http_cache/v3/file_system_http_cache.pb.h"

#include "source/extensions/common/async_files/async_file_manager.h"
#include "source/extensions/filters/http/cache/http_cache.h"

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace FileSystemHttpCache {

using ConfigProto =
    envoy::extensions::http::cache::file_system_http_cache::v3::FileSystemHttpCacheConfig;

class FileSystemHttpCache : public HttpCache,
                            public std::enable_shared_from_this<FileSystemHttpCache>,
                            public Logger::Loggable<Logger::Id::cache_filter> {
public:
  FileSystemHttpCache(Singleton::InstanceSharedPtr owner, ConfigProto config,
                      std::shared_ptr<Common::AsyncFiles::AsyncFileManager>&& async_file_manager);

  // Overrides for HttpCache
  LookupContextPtr makeLookupContext(LookupRequest&& lookup,
                                     Http::StreamDecoderFilterCallbacks& callbacks) override;
  InsertContextPtr makeInsertContext(LookupContextPtr&& lookup_context,
                                     Http::StreamEncoderFilterCallbacks& callbacks) override;
  CacheInfo cacheInfo() const override;

  /**
   * Replaces the headers of a cache entry.
   *
   * To avoid races between readers and writers, this unfortunately must be performed by
   * making a full copy of the original cache entry, and then replacing the original with
   * that new file.
   *
   * For example, if we simply overwrote the headers in place, a reader might be about to
   * read the headers expecting them to have one length, then read them with a different
   * length (or in a partially overwritten state).
   *
   * If we opened the file, unlinked it, modified it, and linked it, a reader might still
   * have already had the original version open and still get the same race as above.
   *
   * Therefore the only way to reliably ensure that there are no readers of the file while
   * it is being written (other than something involving system-wide locks that would have
   * other issues), is to rewrite the contents to a new file. This may be expensive
   * for large cache files.
   *
   * @param lookup_context the lookup context that provoked the updateHeaders call.
   * @param response_headers the http response headers to update the cache entry with.
   * @param metadata the metadata to update the cache entry with.
   * @param on_complete called with true when the entry is updated, or false if not updated.
   */
  void updateHeaders(const LookupContext& lookup_context,
                     const Http::ResponseHeaderMap& response_headers,
                     const ResponseMetadata& metadata,
                     std::function<void(bool)> on_complete) override;

  /**
   * The config of this cache. Used by the factory to ensure there aren't incompatible
   * configs using the same path.
   * @return the config of this cache.
   */
  const ConfigProto& config() { return config_; }

  /**
   * True if the given key currently has a stream writing to it.
   * @param key the key to check against entries_being_written_.
   * @return True if the key is present in entries_being_written_.
   */
  bool workInProgress(const Key& key) ABSL_LOCKS_EXCLUDED(cache_mu_);

  /**
   * Inserts the key into entries_being_written_, excluding it from being written to by
   * another task until the returned Cleanup's destructor is called.
   *
   * If the key is already in entries_being_written_, returns nullptr, indicating that
   * the caller should not continue with the write.
   * @param key the key the caller wishes to start writing to.
   * @return nullptr if a write is already in progress for that key. A Cleanup object
   *     if a write should begin.
   */
  ABSL_MUST_USE_RESULT std::shared_ptr<Cleanup> maybeStartWritingEntry(const Key& key)
      ABSL_LOCKS_EXCLUDED(cache_mu_);

  /**
   * Returns a key of the base key plus vary_identifier, if a vary_identifier can be
   * generated from the inputs. Otherwise returns nullopt.
   * @param base The base key.
   * @param vary_allow_list A vary_allow_list from a LookupContext.
   * @param vary_header_values The vary header values from a vary cache entry.
   * @param request_headers The headers from the LookupContext.
   * @return a key made from the base key plus a vary_identifier, if a vary_identifier
   *     can be generated from the inputs; nullopt if a vary_identifier cannot be
   *     generated.
   */
  static absl::optional<Key>
  makeVaryKey(const Key& base, const VaryAllowList& vary_allow_list,
              const absl::btree_set<absl::string_view>& vary_header_values,
              const Http::RequestHeaderMap& request_headers);

  /**
   * Writes a vary cache file in the background, and inserts `varied_key`
   * into `entries_being_written_` if not already present.
   * @param key the base key for the entry, without vary headers.
   * @param response_headers the headers from the response.
   * @param varied_key the key with the vary header from response_headers incorporated.
   * @param cleanup the Cleanup from the original key, which will be called when the
   *     write operation has completed.
   * @return nullptr if an entry for varied_key was already being written, a Cleanup
   *     if varied_key was added.
   */
  ABSL_MUST_USE_RESULT std::shared_ptr<Cleanup>
  setCacheEntryToVary(const Key& key, const Http::ResponseHeaderMap& response_headers,
                      const Key& varied_key, std::shared_ptr<Cleanup> cleanup)
      ABSL_LOCKS_EXCLUDED(cache_mu_);

  /**
   * Returns the extension name.
   * @return the extension name.
   */
  static absl::string_view name();

  /**
   * Returns a filename for the cache entry with the given key.
   * @param key the key for which to generate a filename.
   * @return a filename for that cache entry (path not included).
   */
  std::string generateFilename(const Key& key) const;

  /**
   * Returns the path for this cache instance.
   * @return the configured path for this cache instance.
   */
  absl::string_view cachePath() const;

  /**
   * Returns the AsyncFileManager associated with this instance.
   * @return a shared_ptr to the AsyncFileManager associated with this instance.
   */
  std::shared_ptr<Common::AsyncFiles::AsyncFileManager> asyncFileManager() const {
    return async_file_manager_;
  }

  // UpdateHeaders copies an existing cache entry to a new file. This value is
  // the size of a copy-chunk. It's public for unit tests only, as the chunk size
  // is totally irrelevant to the outward-facing API.
  static const size_t max_update_headers_copy_chunk_size_;

private:
  /**
   * Writes a vary node to disk for the given key. A vary node in the cache consists of
   * only the vary header.
   * @param key the key at which to write the cache entry.
   * @param response_headers the response headers from which the vary_header_values will
   *     be extracted.
   * @param cleanup the cleanup operation to be performed when the write completes.
   */
  void writeVaryNodeToDisk(const Key& key, const Http::ResponseHeaderMap& response_headers,
                           std::shared_ptr<Cleanup> cleanup);

  // A shared_ptr to keep the cache singleton alive as long as any of its caches are in use.
  const Singleton::InstanceSharedPtr owner_;
  const ConfigProto config_;

  absl::Mutex cache_mu_;
  // When a new cache entry is being written, its key will be here and the cache file
  // will not be present. The cache miss will be detected normally from the filesystem.
  // This should be checked before writing; cancel the write if another thread is already
  // writing the same entry.
  // TODO(ravenblack): if contention of cache_mu_ causes a performance issue, this could
  // be split into multiple hash tables along key boundaries, each with their own mutex.
  // TODO(ravenblack): if detecting cache misses by filesystem causes a performance issue
  // with uncacheable responses, another set could be added to keep track of uncacheable
  // responses, to skip the filesystem hit. (Using ReaderLock should make such a table
  // inherently low contention.)
  absl::flat_hash_set<Key, MessageUtil, MessageUtil>
      entries_being_written_ ABSL_GUARDED_BY(cache_mu_);

  std::shared_ptr<Common::AsyncFiles::AsyncFileManager> async_file_manager_;
};

} // namespace FileSystemHttpCache
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
