#include <bitcoin/blockchain/leveldb_blockchain.hpp>

#include <fstream>

#include <boost/filesystem.hpp>
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>

#include <db_cxx.h>

#include <bitcoin/transaction.hpp>
#include <bitcoin/utility/assert.hpp>
#include <bitcoin/utility/logger.hpp>
#include <bitcoin/utility/serializer.hpp>
#include <bitcoin/format.hpp>

#include "leveldb_common.hpp"
#include "leveldb_chain_keeper.hpp"
#include "leveldb_organizer.hpp"
#include "data_type.hpp"
#include "protobuf_wrapper.hpp"

namespace libbitcoin {

leveldb_blockchain::leveldb_blockchain(async_service& service)
  : async_strand(service)
{
#ifndef CXX_COMPAT
    env_ = nullptr;
    db_blocks_ = nullptr;
    db_blocks_hash_ = nullptr;
    db_txs_ = nullptr;
    db_spends_ = nullptr;
    db_address_ = nullptr;
#endif
    reorganize_subscriber_ =
        std::make_shared<reorganize_subscriber_type>(service);
}
leveldb_blockchain::~leveldb_blockchain()
{
}

void leveldb_blockchain::start(const std::string& prefix,
    start_handler handle_start)
{
    queue(
        [this, prefix, handle_start]
        {
            if (initialize(prefix))
                handle_start(std::error_code());
            else
                handle_start(error::operation_failed);
        });
}
void leveldb_blockchain::stop()
{
    reorganize_subscriber_->relay(error::service_stopped,
        0, block_list(), block_list());
    db_blocks_l1.reset();
    db_blocks_hash_l1.reset();
    db_txs_l1.reset();
    db_spends_l1.reset();
    db_address_l1.reset();
    // delete
    google::protobuf::ShutdownProtobufLibrary();
}

bool open_db(const std::string& prefix, const std::string& db_name,
    std::unique_ptr<leveldb::DB>& db, leveldb::Options open_options)
{
    using boost::filesystem::path;
    path db_path = path(prefix) / db_name;
    leveldb::DB* db_base_ptr = nullptr;
    leveldb::Status status =
        leveldb::DB::Open(open_options, db_path.native(), &db_base_ptr);
    if (!status.ok())
    {
        log_fatal() << "Internal error opening '" << db_name << "' database: "
            << status.ToString();
        return false;
    }
    // The cointainer ensures db_base_ptr is now managed.
    db.reset(db_base_ptr);
    return true;
}

bool leveldb_blockchain::initialize(const std::string& prefix)
{
    using boost::filesystem::path;
    // Try to lock the directory first
    path lock_path = path(prefix) / "db-lock";
    std::ofstream touch_file(lock_path.native(), std::ios::app);
    touch_file.close();
    flock_ = lock_path.c_str();
    if (!flock_.try_lock())
    {
        // Database already opened elsewhere
        return false;
    }
    // Continue on
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    // Open LevelDB databases
    const size_t cache_size = 1 << 20;
    open_options_.block_cache = leveldb::NewLRUCache(cache_size / 2);
    open_options_.write_buffer_size = cache_size / 4;
    open_options_.filter_policy = leveldb::NewBloomFilterPolicy(10);
    open_options_.compression = leveldb::kNoCompression;
    open_options_.max_open_files = 64;
    open_options_.create_if_missing = true;
    if (!open_db(prefix, "blocks", db_blocks_l1, open_options_))
        return false;
    if (!open_db(prefix, "blocks_hash", db_blocks_hash_l1, open_options_))
        return false;
    if (!open_db(prefix, "txs", db_txs_l1, open_options_))
        return false;
    if (!open_db(prefix, "spends", db_spends_l1, open_options_))
        return false;
    if (!open_db(prefix, "address", db_address_l1, open_options_))
        return false;
    // G++ has an internal compiler error when you use the implicit * cast.
    common_ = std::make_shared<leveldb_common>(
        db_blocks_l1.get(), db_blocks_hash_l1.get(),
        db_txs_l1.get(), db_spends_l1.get(), db_address_l1.get());
    // Validate and organisation components.
    //orphans_ = std::make_shared<orphans_pool>(20);
    //leveldb_chain_keeper_ptr chainkeeper = 
    //    std::make_shared<leveldb_chain_keeper>(common_, env_,
    //        db_blocks_, db_blocks_hash_, db_txs_, db_spends_, db_address_);
    //chain_ = chainkeeper;
    //organize_ = std::make_shared<leveldb_organizer>(
    //    common_, orphans_, chainkeeper, reorganize_subscriber_);
    return true;
}

void leveldb_blockchain::store(const message::block& stored_block, 
    store_block_handler handle_store)
{
    queue(
        std::bind(&leveldb_blockchain::do_store,
            this, stored_block, handle_store));
}
void leveldb_blockchain::do_store(const message::block& stored_block,
    store_block_handler handle_store)
{
    block_detail_ptr stored_detail =
        std::make_shared<block_detail>(stored_block);
    int depth = chain_->find_index(hash_block_header(stored_block));
    if (depth != -1)
    {
        handle_store(error::duplicate,
            block_info{block_status::confirmed, depth});
        return;
    }
    if (!orphans_->add(stored_detail))
    {
        handle_store(error::duplicate,
            block_info{block_status::orphan, 0});
    }
    organize_->start();
    handle_store(stored_detail->errc(), stored_detail->info());
    // Every N blocks, we flush database
    static size_t flush_counter = 0;
    if (++flush_counter == 2000)
    {
        env_->txn_checkpoint(0, 0, 0);
        flush_counter = 0;
    }
}

void leveldb_blockchain::import(const message::block& import_block,
    size_t depth, import_block_handler handle_import)
{
    queue(
        std::bind(&leveldb_blockchain::do_import,
            this, import_block, depth, handle_import));
}
void leveldb_blockchain::do_import(const message::block& import_block,
    size_t depth, import_block_handler handle_import)
{
    if (!common_->save_block(depth, import_block))
    {
        handle_import(error::operation_failed);
        return;
    }
    handle_import(std::error_code());
}

template<typename Index>
bool fetch_block_header_impl(const Index& index,
    leveldb_common_ptr common, message::block& serial_block)
{
    protobuf::Block proto_block = common->fetch_proto_block(index);
    if (!proto_block.IsInitialized())
        return false;
    serial_block = protobuf_to_block_header(proto_block);
    return true;
} 

void leveldb_blockchain::fetch_block_header(size_t depth,
    fetch_handler_block_header handle_fetch)
{
    queue(
        std::bind(&leveldb_blockchain::fetch_block_header_by_depth,
            this, depth, handle_fetch));
}

void leveldb_blockchain::fetch_block_header_by_depth(size_t depth,
    fetch_handler_block_header handle_fetch)
{
    message::block serial_block;
    if (!fetch_block_header_impl(depth, common_, serial_block))
    {
        handle_fetch(error::not_found, message::block());
        return;
    }
    handle_fetch(std::error_code(), serial_block);
}

void leveldb_blockchain::fetch_block_header(const hash_digest& block_hash,
    fetch_handler_block_header handle_fetch)
{
    queue(
        std::bind(&leveldb_blockchain::fetch_block_header_by_hash,
            this, block_hash, handle_fetch));
}

void leveldb_blockchain::fetch_block_header_by_hash(
    const hash_digest& block_hash, fetch_handler_block_header handle_fetch)
{
    message::block serial_block;
    if (!fetch_block_header_impl(block_hash, common_, serial_block))
    {
        handle_fetch(error::not_found, message::block());
        return;
    }
    handle_fetch(std::error_code(), serial_block);
}

template<typename Index, typename Handler>
void fetch_blk_tx_hashes_impl(const Index& index,
    leveldb_common_ptr common, Handler handle_fetch)
{
    protobuf::Block proto_block = common->fetch_proto_block(index);
    if (!proto_block.IsInitialized())
    {
        handle_fetch(error::not_found, message::inventory_list());
        return;
    }
    message::inventory_list tx_hashes;
    for (const std::string& raw_tx_hash: proto_block.transactions())
    {
        message::inventory_vector tx_inv;
        tx_inv.type = message::inventory_type::transaction;
        BITCOIN_ASSERT(raw_tx_hash.size() == tx_inv.hash.size());
        std::copy(raw_tx_hash.begin(), raw_tx_hash.end(),
            tx_inv.hash.begin());
        tx_hashes.push_back(tx_inv);
    }
    handle_fetch(std::error_code(), tx_hashes);
}

void leveldb_blockchain::fetch_block_transaction_hashes(size_t depth,
    fetch_handler_block_transaction_hashes handle_fetch)
{
    queue(
        [this, depth, handle_fetch]
        {
            fetch_blk_tx_hashes_impl(depth, common_, handle_fetch);
        });
}

void leveldb_blockchain::fetch_block_transaction_hashes(
    const hash_digest& block_hash,
    fetch_handler_block_transaction_hashes handle_fetch)
{
    queue(
        [this, block_hash, handle_fetch]
        {
            fetch_blk_tx_hashes_impl(block_hash, common_, handle_fetch);
        });
}

void leveldb_blockchain::fetch_block_depth(const hash_digest& block_hash,
    fetch_handler_block_depth handle_fetch)
{
    queue(
        std::bind(&leveldb_blockchain::do_fetch_block_depth,
            this, block_hash, handle_fetch));
}
void leveldb_blockchain::do_fetch_block_depth(const hash_digest& block_hash,
    fetch_handler_block_depth handle_fetch)
{
    uint32_t depth = common_->fetch_block_depth(block_hash);
    if (depth == std::numeric_limits<uint32_t>::max())
        handle_fetch(error::not_found, 0);
    else
        handle_fetch(std::error_code(), depth);
}

void leveldb_blockchain::fetch_last_depth(
    fetch_handler_last_depth handle_fetch)
{
    queue(
        std::bind(&leveldb_blockchain::do_fetch_last_depth,
            this, handle_fetch));
}
void leveldb_blockchain::do_fetch_last_depth(
    fetch_handler_last_depth handle_fetch)
{
    uint32_t last_depth = common_->find_last_block_depth();
    if (last_depth == std::numeric_limits<uint32_t>::max())
    {
        handle_fetch(error::not_found, 0);
        return;
    }
    handle_fetch(std::error_code(), last_depth);
}

void leveldb_blockchain::fetch_transaction(
    const hash_digest& transaction_hash,
    fetch_handler_transaction handle_fetch)
{
    queue(
        std::bind(&leveldb_blockchain::do_fetch_transaction,
            this, transaction_hash, handle_fetch));
}
void leveldb_blockchain::do_fetch_transaction(
    const hash_digest& transaction_hash,
    fetch_handler_transaction handle_fetch)
{
    protobuf::Transaction proto_tx =
        common_->fetch_proto_transaction(transaction_hash);
    if (!proto_tx.IsInitialized())
    {
        handle_fetch(error::not_found, message::transaction());
        return;
    }
    message::transaction tx = protobuf_to_transaction(proto_tx);
    handle_fetch(std::error_code(), tx);
}

void leveldb_blockchain::fetch_transaction_index(
    const hash_digest& transaction_hash,
    fetch_handler_transaction_index handle_fetch)
{
    queue(
        std::bind(&leveldb_blockchain::do_fetch_transaction_index,
            this, transaction_hash, handle_fetch));
}
void leveldb_blockchain::do_fetch_transaction_index(
    const hash_digest& transaction_hash,
    fetch_handler_transaction_index handle_fetch)
{
    protobuf::Transaction proto_tx =
        common_->fetch_proto_transaction(transaction_hash);
    if (!proto_tx.IsInitialized())
    {
        handle_fetch(error::not_found, 0, 0);
        return;
    }
    handle_fetch(std::error_code(),
        proto_tx.parent().depth(), proto_tx.parent().index());
}

void leveldb_blockchain::fetch_spend(const message::output_point& outpoint,
    fetch_handler_spend handle_fetch)
{
    queue(
        std::bind(&leveldb_blockchain::do_fetch_spend,
            this, outpoint, handle_fetch));
}
void leveldb_blockchain::do_fetch_spend(
    const message::output_point& outpoint,
    fetch_handler_spend handle_fetch)
{
    message::input_point input_spend;
    if (!common_->fetch_spend(outpoint, input_spend))
    {
        handle_fetch(error::unspent_output, message::input_point());
        return;
    }
    handle_fetch(std::error_code(), input_spend);
}

void leveldb_blockchain::fetch_outputs(const payment_address& address,
    fetch_handler_outputs handle_fetch)
{
    if (address.type() != payment_type::pubkey_hash)
        handle_fetch(error::unsupported_payment_type,
            message::output_point_list());
    else
        queue(
            std::bind(&leveldb_blockchain::do_fetch_outputs,
                this, address, handle_fetch));
}
void leveldb_blockchain::do_fetch_outputs(const payment_address& address,
    fetch_handler_outputs handle_fetch)
{
    // Associated outputs
    message::output_point_list assoc_outs;
    // version byte + hash for key
    serializer serial;
    serial.write_byte(address.version());
    serial.write_short_hash(address.hash());
    data_chunk raw_address = serial.data();
    // Fetch outpoints as contiguous block.
    std::string outpoints;
    leveldb::Status status = db_address_l1->Get(
        leveldb::ReadOptions(), slice(raw_address), &outpoints);
    if (!status.ok())
    {
        if (status.IsNotFound())
            handle_fetch(error::not_found, message::output_point_list());
        else
        {
            handle_fetch(error::operation_failed,
                message::output_point_list());
            log_error() << "Error fetch_outputs: " << status.ToString();
        }
        return;
    }
    // Must be a multiple of (32 + 4)
    const size_t outpoint_size = 32 + 4;
    BITCOIN_ASSERT(outpoints.size() % outpoint_size == 0);
    for (auto it = outpoints.begin(); it != outpoints.end();
        it += outpoint_size)
    {
        // We need a copy not a temporary
        data_chunk raw_outpoint(it, it + outpoint_size);
        // Then read the value off
        deserializer deserial(raw_outpoint);
        message::output_point outpoint;
        outpoint.hash = deserial.read_hash();
        outpoint.index = deserial.read_4_bytes();
        assoc_outs.push_back(outpoint);
    }
    handle_fetch(std::error_code(), assoc_outs);
}

void leveldb_blockchain::subscribe_reorganize(
    reorganize_handler handle_reorganize)
{
    reorganize_subscriber_->subscribe(handle_reorganize);
}

} // namespace libbitcoin

