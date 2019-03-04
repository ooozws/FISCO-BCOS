/*
 * @CopyRight:
 * FISCO-BCOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FISCO-BCOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FISCO-BCOS.  If not, see <http://www.gnu.org/licenses/>
 * (c) 2016-2018 fisco-dev contributors.
 */
/** @file MinerPrecompiled.h
 *  @author ancelmo
 *  @date 20180921
 */

#include "LevelDBStorage.h"
#include "Table.h"
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <libdevcore/Guards.h>
#include <libdevcore/easylog.h>
#include <omp.h>
#include <memory>
#include <thread>

using namespace dev;
using namespace dev::storage;

Entries::Ptr LevelDBStorage::select(h256, int, const std::string& table, const std::string& key)
{
    try
    {
        std::string entryKey = table;
        entryKey.append("_").append(key);

        std::string value;
        // ReadGuard l(m_remoteDBMutex);
        auto s = m_db->Get(leveldb::ReadOptions(), leveldb::Slice(entryKey), &value);
        // l.unlock();
        if (!s.ok() && !s.IsNotFound())
        {
            STORAGE_LEVELDB_LOG(ERROR)
                << LOG_DESC("Query leveldb failed") << LOG_KV("status", s.ToString());

            BOOST_THROW_EXCEPTION(StorageException(-1, "Query leveldb exception:" + s.ToString()));
        }

        Entries::Ptr entries = std::make_shared<Entries>();
        if (!s.IsNotFound())
        {
            // parse json
            std::stringstream ssIn;
            ssIn << value;

            Json::Value valueJson;
            ssIn >> valueJson;

            Json::Value values = valueJson["values"];
            for (auto it = values.begin(); it != values.end(); ++it)
            {
                Entry::Ptr entry = std::make_shared<Entry>();

                for (auto valueIt = it->begin(); valueIt != it->end(); ++valueIt)
                {
                    entry->setField(valueIt.key().asString(), valueIt->asString());
                }

                if (entry->getStatus() == Entry::Status::NORMAL)
                {
                    entry->setDirty(false);
                    entries->addEntry(entry);
                }
            }
        }

        return entries;
    }
    catch (std::exception& e)
    {
        STORAGE_LEVELDB_LOG(ERROR) << LOG_DESC("Query leveldb exception")
                                   << LOG_KV("msg", boost::diagnostic_information(e));

        BOOST_THROW_EXCEPTION(e);
    }

    return Entries::Ptr();
}

size_t LevelDBStorage::commitTableDataRange(std::shared_ptr<dev::db::LevelDBWriteBatch>& batch,
    TableData::Ptr tableData, h256 hash, int64_t num, size_t from, size_t to)
{
    // commit table data of given range, thread safe
    // range to commit: [from, to)
    if (from >= to)
        return 0;
    size_t total = 0;
    auto dataIt = tableData->data.begin();
    std::advance(dataIt, from);
    batch = m_db->createWriteBatch();

    while (from < to && dataIt != tableData->data.end())
    {
        if (dataIt->second->size() == 0u)
        {
            dataIt++;
            from++;
            continue;
        }
        std::string entryKey = tableData->tableName + "_" + dataIt->first;
        Json::Value entry;

        for (size_t i = 0; i < dataIt->second->size(); ++i)
        {
            Json::Value value;
            for (auto& fieldIt : *(dataIt->second->get(i)->fields()))
            {
                value[fieldIt.first] = fieldIt.second;
            }
            value["_hash_"] = hash.hex();
            value["_num_"] = num;
            entry["values"].append(value);
        }

        std::stringstream ssOut;
        ssOut << entry;

        batch->insertSlice(leveldb::Slice(entryKey), leveldb::Slice(ssOut.str()));
        ++total;
        // ssOut.seekg(0, std::ios::end);
        STORAGE_LEVELDB_LOG(TRACE)
            << LOG_KV("commit key", entryKey) << LOG_KV("entries", dataIt->second->size());
        //<< LOG_KV("len", ssOut.tellg());
        dataIt++;
        from++;
    }

    return total;
}

static const size_t c_commitTableDataRangeEachThread = 128;  // 128 is good after testing
size_t LevelDBStorage::commit(
    h256 hash, int64_t num, const std::vector<TableData::Ptr>& datas, h256 const&)
{
    try
    {
        auto start_time = utcTime();
        auto record_time = utcTime();
        auto encode_time_cost = 0;
        auto writeDB_time_cost = 0;

        std::atomic<size_t> total;
        total = 0;

        for (size_t i = 0; i < datas.size(); ++i)
        {
            TableData::Ptr tableData = datas[i];
            size_t totalSize = tableData->data.size();

            // Parallel encode and batch
            size_t batchesSize = (totalSize + c_commitTableDataRangeEachThread - 1) /
                                 c_commitTableDataRangeEachThread;
            std::vector<std::shared_ptr<dev::db::LevelDBWriteBatch>> batches(batchesSize, nullptr);
#pragma omp parallel for
            for (size_t j = 0; j < batchesSize; ++j)
            {
                size_t from = c_commitTableDataRangeEachThread * j;
                size_t to = std::min(c_commitTableDataRangeEachThread * (j + 1), totalSize);
                size_t threadTotal =
                    commitTableDataRange(batches[j], tableData, hash, num, from, to);
                total += threadTotal;
            }
            encode_time_cost += utcTime() - record_time;
            record_time = utcTime();

            // write batch
            std::shared_ptr<dev::db::LevelDBWriteBatch> totalBatch = m_db->createWriteBatch();
            for (size_t j = 0; j < batchesSize; ++j)
            {
                totalBatch->append(*batches[j]);
            }
            leveldb::WriteOptions writeOptions;
            writeOptions.sync = false;
            auto s = m_db->Write(writeOptions, &(totalBatch->writeBatch()));

            if (!s.ok())
            {
                STORAGE_LEVELDB_LOG(FATAL) << LOG_DESC(
                                                  "Commit leveldb crashed! Please remove all "
                                                  "data and sync data from other nodes")
                                           << LOG_KV("status", s.ToString());

                BOOST_THROW_EXCEPTION(
                    StorageException(-1, "Commit leveldb exception:" + s.ToString()));
                return 0;
            }
            writeDB_time_cost += utcTime() - record_time;
            record_time = utcTime();
        }
        STORAGE_LEVELDB_LOG(DEBUG) << LOG_BADGE("Commit") << LOG_DESC("Write to db")
                                   << LOG_KV("encodeTimeCost", encode_time_cost)
                                   << LOG_KV("writeDBTimeCost", writeDB_time_cost)
                                   << LOG_KV("totalTimeCost", utcTime() - start_time);

        return total.load();
    }
    catch (std::exception& e)
    {
        STORAGE_LEVELDB_LOG(ERROR) << LOG_DESC("Commit leveldb exception")
                                   << LOG_KV("msg", boost::diagnostic_information(e));
        BOOST_THROW_EXCEPTION(e);
    }

    return 0;
}

bool LevelDBStorage::onlyDirty()
{
    return false;
}

void LevelDBStorage::setDB(std::shared_ptr<dev::db::BasicLevelDB> db)
{
    m_db = db;
}
