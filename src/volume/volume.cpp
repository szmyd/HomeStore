
//
// Created by Kadayam, Hari on 06/11/17.
//

#include "volume.hpp"
#include <mapping/mapping.hpp>

using namespace std;

#define MAX_CACHE_SIZE     (100 * 1024ul * 1024ul) /* it has to be a multiple of 16k */
constexpr auto BLOCK_SIZE = (4 * 1024ul);

static std::map<std::string, std::shared_ptr<homestore::Volume>> volume_map;
static std::mutex map_lock;

#ifndef NDEBUG
/* only for testing */
bool vol_test_enable = false;
#endif
/* TODO: it will be more cleaner once statisitcs is integrated */
std::atomic<int> homestore::req_alloc(0);
std::atomic<int> homestore::req_dealloc(0);
int btree_buf_alloc;
int btree_buf_free;
int btree_buf_make_obj;

using namespace homestore;

std::shared_ptr<Volume>
Volume::createVolume(std::string const& uuid,
        DeviceManager* mgr,
        uint64_t const size,
        comp_callback comp_cb) {
    decltype(volume_map)::iterator it;
    // Try to add an entry for this volume
    {  std::lock_guard<std::mutex> lg (map_lock);
        bool happened {false};
        std::tie(it, happened) = volume_map.emplace(std::make_pair(uuid, nullptr));
        if (!happened) {
            if (volume_map.end() != it) return it->second;
            throw std::runtime_error("Unknown bug");
        }
    }
    // Okay, this is a new volume so let's create it
    auto new_vol = new Volume(mgr, size, comp_cb);
    it->second.reset(new_vol);
    return it->second;
}

std::error_condition
Volume::removeVolume(std::string const& uuid) {
   std::shared_ptr<Volume> volume;
   // Locked Map
   { std::lock_guard<std::mutex> lg(map_lock);
      if (auto it = volume_map.find(uuid); volume_map.end() != it) {
         if (2 <= it->second.use_count()) {
            LOGERROR("Refusing to delete volume with outstanding references: {}", uuid);
            return std::make_error_condition(std::errc::device_or_resource_busy);
         }
         volume = std::move(it->second);
         volume_map.erase(it);
      }
   } // Unlock Map
   return (volume ? volume->destroy() : std::make_error_condition(std::errc::no_such_device_or_address));
}

std::shared_ptr<Volume>
Volume::lookupVolume(std::string const& uuid) {
    {  std::lock_guard<std::mutex> lg (map_lock);
        auto it = volume_map.find(uuid);
        if (volume_map.end() != it) return it->second;
    }
    return nullptr;
}

Cache< BlkId > * Volume::glob_cache = NULL;
uint64_t 
Volume::get_elapsed_time(Clock::time_point startTime) {
    std::chrono::nanoseconds ns = std::chrono::duration_cast
        < std::chrono::nanoseconds >(Clock::now() - startTime);
    return ns.count() / 1000;
}

AbstractVirtualDev *
Volume::new_vdev_found(DeviceManager *dev_mgr, vdev_info_block *vb) {
    LOGINFO("New virtual device found id = {} size = {}", vb->vdev_id, vb->size);

    /* TODO: enable it after testing */
#if 0
    Volume *volume = new Volume(dev_mgr, vb);
    return volume->blk_store->get_vdev();
#endif
    return NULL;
}

Volume::Volume(DeviceManager *dev_mgr, uint64_t size,
        comp_callback comp_cb):comp_cb(comp_cb) {
    fLI::FLAGS_minloglevel=3;
    if (Volume::glob_cache == NULL) {
        Volume::glob_cache = new Cache< BlkId >(MAX_CACHE_SIZE, BLOCK_SIZE);
        cout << "cache created\n";
    }
    blk_store = new BlkStore< VdevVarSizeBlkAllocatorPolicy >
        (dev_mgr, Volume::glob_cache, size,
         WRITEBACK_CACHE, 0,
         (std::bind(&Volume::process_data_completions, 
                    this, std::placeholders::_1)));
    map = new mapping(size, 
            [this] (homestore::BlkId bid) { free_blk(bid); }, 
            (std::bind(&Volume::process_metadata_completions, this, 
                std::placeholders::_1)), dev_mgr, 
                Volume::glob_cache);
    alloc_single_block_in_mem();
    init_perf_cntrs();
}

Volume::Volume(DeviceManager *dev_mgr, vdev_info_block *vb) {
    size = vb->size;
    if (Volume::glob_cache == NULL) {
        Volume::glob_cache = new Cache< BlkId >(MAX_CACHE_SIZE, BLOCK_SIZE);
        cout << "cache created\n";
    }
    blk_store = new BlkStore< VdevVarSizeBlkAllocatorPolicy >
        (dev_mgr, Volume::glob_cache, vb, 
         WRITEBACK_CACHE, 
         (std::bind(&Volume::process_data_completions, this,
                    std::placeholders::_1)));
    map = new mapping(size, 
            [this] (homestore::BlkId bid) { free_blk(bid); }, 
            (std::bind(&Volume::process_metadata_completions, this, 
                std::placeholders::_1)), dev_mgr, 
                Volume::glob_cache);
    alloc_single_block_in_mem();
    /* TODO: rishabh, We need a attach function to register completion callback if layers
     * are called from bottomup.
     */
    init_perf_cntrs();
}

std::error_condition
Volume::destroy() {
   LOGWARN("UnImplemented volume destruction!");
   return std::error_condition();
}

atomic<uint64_t> total_read_finish_ios;
atomic<uint64_t>   total_read_sent_ios;

void 
homestore::Volume::process_metadata_completions(boost::intrusive_ptr<volume_req> req) {
    assert(!req->is_read);
    if (req->err == no_error) {
        io_write_time.fetch_add(get_elapsed_time(req->startTime), memory_order_relaxed);
    }
    comp_cb(req);
    outstanding_write_cnt.fetch_add(1, memory_order_relaxed);
}

void 
homestore::Volume::process_data_completions(boost::intrusive_ptr<blkstore_req<BlkBuffer>> bs_req) {

    boost::intrusive_ptr< volume_req > req = boost::static_pointer_cast< volume_req >(bs_req);
    if (!req->is_read) {
        return;
    }
    for (unsigned int i = 0; i < req->read_buf_list.size(); i++) {
        if (req->read_buf_list[i].buf == only_in_mem_buff) {
            continue;
        }
        blk_store->update_cache(req->read_buf_list[i].buf);
    }
    io_read_time.fetch_add(get_elapsed_time(req->startTime), memory_order_relaxed);
    comp_cb(req);
}

void
Volume::init_perf_cntrs() {
    read_cnt = 0;
    write_cnt = 0;
    alloc_blk_time = 0;
    write_time = 0;
    map_time = 0;
    io_write_time = 0;
    outstanding_write_cnt = 0;
    blk_store->init_perf_cnts();
}

void
Volume::print_perf_cntrs() {
    printf("avg time taken in alloc_blk %lu us\n", alloc_blk_time/write_cnt);
    printf("avg time taken in issuing write from volume layer %lu us\n", 
            write_time/write_cnt);
    printf("avg time taken in writing map %lu us\n", map_time/write_cnt);
    printf("avg time taken in write %lu us\n", io_write_time/write_cnt);
    if (atomic_load(&read_cnt) != 0) {
        printf("avg time taken in read %lu us\n", io_read_time/read_cnt);
        printf("avg time taken in reading map %lu us\n", 
                map_read_time/read_cnt);
        printf("avg time taken in issuing read from volume layer %lu us\n", 
                read_time/read_cnt);
    }
    blk_store->print_perf_cnts();
}

void
homestore::Volume::free_blk(homestore::BlkId bid) {
    blk_store->free_blk(bid, boost::none, boost::none);
}

boost::intrusive_ptr< BlkBuffer > 
Volume::write(uint64_t lba, uint8_t *buf, uint32_t nblks, 
                                boost::intrusive_ptr<volume_req> req) {
    BlkId bid;
    blk_alloc_hints hints;
    hints.desired_temp = 0;
    hints.dev_id_hint = -1;

    req->lba = lba;
    req->nblks = nblks;
    req->is_read = false;
    req->startTime = Clock::now();
    req->err = no_error;

    write_cnt.fetch_add(1, memory_order_relaxed);
    {
        Clock::time_point startTime = Clock::now();
        BlkAllocStatus status = blk_store->alloc_blk(nblks, hints, &bid);
        if (status != BLK_ALLOC_SUCCESS) {
            assert(0);
        }
        alloc_blk_time.fetch_add(get_elapsed_time(startTime), memory_order_relaxed);
    }
    req->bid = bid;

    // LOG(INFO) << "Requested nblks: " << (uint32_t) nblks << " Allocation info: " << bid.to_string();

    homeds::blob b = {buf, (uint32_t)(BLOCK_SIZE * nblks)};

    Clock::time_point startTime = Clock::now();

    std::deque<boost::intrusive_ptr<writeback_req>> req_q;
    boost::intrusive_ptr< BlkBuffer > bbuf = blk_store->write(bid, b, 
                                 boost::static_pointer_cast<blkstore_req<BlkBuffer>>(req), req_q);

    /* TODO: should check the write status */
    write_time.fetch_add(get_elapsed_time(startTime), memory_order_relaxed);
    //  LOG(INFO) << "Written on " << bid.to_string() << " for 8192 bytes";
    map->put(req, req->lba, req->nblks, req->bid);
    return bbuf;
}

void Volume::print_tree(){
    map->print_tree();
}

int
Volume::read(uint64_t lba, int nblks, boost::intrusive_ptr<volume_req> req) {

    std::vector< struct lba_BlkId_mapping > mappingList;
    req->startTime = Clock::now();
    Clock::time_point startTime = Clock::now();

    std::error_condition ret = map->get(lba, nblks, mappingList);
    req->err = ret;
    req->is_read = true;
    if (ret && ret == homestore_error::lba_not_exist) {
        process_data_completions(req); 
        return 0;
    }


    read_cnt.fetch_add(1, memory_order_relaxed);
    map_read_time.fetch_add(get_elapsed_time(startTime), memory_order_relaxed);

    req->lba = lba;
    req->nblks = nblks;

    req->blkstore_read_cnt = 1;

    startTime = Clock::now();
    for (auto bInfo: mappingList) {
        if (!bInfo.blkid_found) {
            uint8_t i = 0;
            while (i < bInfo.blkId.get_nblks()) {
                buf_info info;
                info.buf = only_in_mem_buff;
                info.size = BLOCK_SIZE;
                info.offset = 0;
                req->read_buf_list.push_back(info);
                i++;
            }
        } else {
            boost::intrusive_ptr<BlkBuffer> bbuf = blk_store->read(bInfo.blkId, 
                    0, BLOCK_SIZE * bInfo.blkId.get_nblks(), 
                    boost::static_pointer_cast<blkstore_req<BlkBuffer>>(req));
            buf_info info;
            info.buf = bbuf;
            info.size = bInfo.blkId.get_nblks() * BLOCK_SIZE;
            info.offset = bInfo.blkId.get_id() - bbuf->get_key().get_id();
            req->read_buf_list.push_back(info);

        }
    }

    int cnt = req->blkstore_read_cnt.fetch_sub(1, std::memory_order_acquire);
    if (cnt == 1) {
        process_data_completions(req);
    }

    read_time.fetch_add(get_elapsed_time(startTime), memory_order_relaxed);
    return 0;
}

/* Just create single block in memory, not on physical device and not in cache */
void Volume::alloc_single_block_in_mem() {
    BlkId *out_blkid = new BlkId(0);
    // Create an object for the buffer
    only_in_mem_buff = BlkBuffer::make_object();
    only_in_mem_buff->set_key(*out_blkid);

    // Create a new block of memory for the blocks requested and set the memvec pointer to that
    uint8_t *ptr;
    uint32_t size = BLKSTORE_BLK_SIZE;
    int ret = posix_memalign((void **) &ptr, 4096, size); // TODO: Align based on hw needs instead of 4k
    if (ret != 0) {
        throw std::bad_alloc();
    }
    memset(ptr, 0, size);
    homeds::MemVector< BLKSTORE_BLK_SIZE > &mvec = only_in_mem_buff->get_memvec_mutable();
    mvec.set(ptr, size, 0);
}
