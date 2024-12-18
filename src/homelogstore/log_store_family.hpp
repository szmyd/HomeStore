/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

#include <sisl/fds/buffer.hpp>
#include <folly/Synchronized.h>

#include "logstore_header.hpp"
#include "log_dev.hpp"

namespace homestore {
struct log_dump_req;

struct logstore_info_t {
    std::shared_ptr< HomeLogStore > m_log_store;
    log_store_opened_cb_t m_on_log_store_opened;
    bool append_mode;
};

struct truncate_req;
class LogStoreFamily {
    friend class HomeLogStoreMgr;
    friend class LogDev;

public:
    LogStoreFamily(const logstore_family_id_t f_id);
    LogStoreFamily(const LogStoreFamily&) = delete;
    LogStoreFamily(LogStoreFamily&&) noexcept = delete;
    LogStoreFamily& operator=(const LogStoreFamily&) = delete;
    LogStoreFamily& operator=(LogStoreFamily&&) noexcept = delete;

    void start(const bool format, JournalVirtualDev* blk_store);
    void stop();

    [[nodiscard]] std::shared_ptr< HomeLogStore > create_new_log_store(const bool append_mode = false);
    void open_log_store(const logstore_id_t store_id, const bool append_mode, const log_store_opened_cb_t& on_open_cb);
    [[nodiscard]] bool close_log_store(const logstore_id_t store_id) {
        // TODO: Implement this method
        return true;
    }
    void remove_log_store(const logstore_id_t store_id);

    void device_truncate_in_user_reactor(const std::shared_ptr< truncate_req >& treq);

    [[nodiscard]] nlohmann::json dump_log_store(const log_dump_req& dum_req);
    std::shared_ptr< HomeLogStore > find_logstore_by_id(logstore_id_t store_id);

    LogDev& logdev() { return m_log_dev; }

    sisl::status_response get_status(const sisl::status_request& request) const;
    sisl::sobject_ptr sobject() { return m_sobject; }

    std::string get_name() const { return m_name; }

    [[nodiscard]] logdev_key do_device_truncate(const bool dry_run = false);


private:

    void on_log_store_found(const logstore_id_t store_id, const logstore_superblk& meta);
    void on_io_completion(const logstore_id_t id, const logdev_key ld_key, const logdev_key flush_idx,
                          const uint32_t nremaining_in_batch, void* const ctx);
    void on_logfound(const logstore_id_t id, const logstore_seq_num_t seq_num, const logdev_key ld_key,
                     const logdev_key flush_ld_key, const log_buffer buf, const uint32_t nremaining_in_batch);
    void on_batch_completion(HomeLogStore* log_store, const uint32_t nremaining_in_batch,
                             const logdev_key flush_ld_key);

public:
    folly::Synchronized< std::unordered_map< logstore_id_t, logstore_info_t > > m_id_logstore_map;
    std::unordered_map< logstore_id_t, uint64_t > m_unopened_store_io;
    std::unordered_set< logstore_id_t > m_unopened_store_id;
    std::unordered_map< logstore_id_t, logid_t > m_last_flush_info;
    logstore_family_id_t m_family_id;
    std::string m_name;
    LogDev m_log_dev;
    sisl::sobject_ptr m_sobject;
};
} // namespace homestore
