#include <list>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <string>
#include <set>
#include <map>
#include <limits>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/error/en.h"
#include "glog/logging.h"
#include "tendisplus/replication/repl_manager.h"
#include "tendisplus/storage/record.h"
#include "tendisplus/commands/command.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/utils/redis_port.h"
#include "tendisplus/utils/invariant.h"

namespace tendisplus {

void ReplManager::supplyFullSync(asio::ip::tcp::socket sock,
                        const std::string& storeIdArg) {
    std::shared_ptr<BlockingTcpClient> client =
        std::move(_svr->getNetwork()->createBlockingClient(
            std::move(sock), 64*1024*1024));

    // NOTE(deyukong): this judge is not precise
    // even it's not full at this time, it can be full during schedule.
    if (isFullSupplierFull()) {
        client->writeLine("-ERR workerpool full", std::chrono::seconds(1));
        return;
    }

    Expected<int64_t> expStoreId = stoul(storeIdArg);
    if (!expStoreId.ok() || expStoreId.value() < 0) {
        client->writeLine("-ERR invalid storeId", std::chrono::seconds(1));
        return;
    }
    uint32_t storeId = static_cast<uint32_t>(expStoreId.value());
    _fullPusher->schedule([this, storeId, client(std::move(client))]() mutable {
        supplyFullSyncRoutine(std::move(client), storeId);
    });
}

bool ReplManager::isFullSupplierFull() const {
    return _fullPusher->isFull();
}

void ReplManager::masterPushRoutine(uint32_t storeId, uint64_t clientId) {
    SCLOCK::time_point nextSched = SCLOCK::now();
    auto guard = MakeGuard([this, &nextSched, storeId, clientId] {
        std::lock_guard<std::mutex> lk(_mutex);
        auto& mpov = _pushStatus[storeId];
        if (mpov.find(clientId) == mpov.end()) {
            return;
        }
        INVARIANT(mpov[clientId]->isRunning);
        mpov[clientId]->isRunning = false;
        mpov[clientId]->nextSchedTime = nextSched;
        // currently nothing waits for master's push process
        // _cv.notify_all();
    });

    uint64_t binlogPos = 0;
    BlockingTcpClient *client = nullptr;
    uint32_t dstStoreId = 0;
    {
        std::lock_guard<std::mutex> lk(_mutex);
        if (_pushStatus[storeId].find(clientId) == _pushStatus[storeId].end()) {
            nextSched = nextSched + std::chrono::seconds(1);
            return;
        }
        binlogPos = _pushStatus[storeId][clientId]->binlogPos;
        client = _pushStatus[storeId][clientId]->client.get();
        dstStoreId = _pushStatus[storeId][clientId]->dstStoreId;
    }

    Expected<uint64_t> newPos = masterSendBinlog(client, storeId, dstStoreId, binlogPos);
    if (!newPos.ok()) {
        LOG(WARNING) << "masterSendBinlog to client:"
                << client->getRemoteRepr() << " failed:"
                << newPos.status().toString();
        std::lock_guard<std::mutex> lk(_mutex);
        // it is safe to remove a non-exist key
        _pushStatus[storeId].erase(clientId);
        return;
    } else {
        std::lock_guard<std::mutex> lk(_mutex);
        _pushStatus[storeId][clientId]->binlogPos = newPos.value();
        if (newPos.value() > binlogPos) {
            nextSched = SCLOCK::now();
        } else {
            nextSched = SCLOCK::now() + std::chrono::seconds(1);
        }
    }
}

Expected<uint64_t> ReplManager::masterSendBinlog(BlockingTcpClient* client,
                uint32_t storeId, uint32_t dstStoreId, uint64_t binlogPos) {
    constexpr uint32_t suggestBatch = 64;
    constexpr size_t suggestBytes = 16*1024*1024;
    PStore store = _svr->getSegmentMgr()->getInstanceById(storeId);
    INVARIANT(store != nullptr);

    auto ptxn = store->createTransaction();
    if (!ptxn.ok()) {
        return ptxn.status();
    }

    std::unique_ptr<Transaction> txn = std::move(ptxn.value());
    std::unique_ptr<BinlogCursor> cursor =
        txn->createBinlogCursor(binlogPos+1);

    std::vector<ReplLog> binlogs;
    uint32_t cnt = 0;
    uint64_t nowId = 0;
    size_t estimateSize = 0;

    while (true) {
        Expected<ReplLog> explog = cursor->next();
        if (explog.ok()) {
            cnt += 1;
            const ReplLogKey& rlk = explog.value().getReplLogKey();
            const ReplLogValue& rlv = explog.value().getReplLogValue();
            estimateSize += rlv.getOpValue().size();
            if (nowId == 0 || nowId != rlk.getTxnId()) {
                nowId = rlk.getTxnId();
                if (cnt >= suggestBatch || estimateSize >= suggestBytes) {
                    break;
                } else {
                    binlogs.emplace_back(std::move(explog.value()));
                }
            } else {
                binlogs.emplace_back(std::move(explog.value()));
            }
        } else if (explog.status().code() == ErrorCodes::ERR_EXHAUST) {
            break;
        } else {
            LOG(ERROR) << "iter binlog failed:"
                        << explog.status().toString();
            return explog.status();
        }
    }

    std::stringstream ss;
    Command::fmtMultiBulkLen(ss, binlogs.size()*2 + 2);
    Command::fmtBulk(ss, "applybinlogs");
    Command::fmtBulk(ss, std::to_string(dstStoreId));
    for (auto& v : binlogs) {
        ReplLog::KV kv = v.encode();
        Command::fmtBulk(ss, kv.first);
        Command::fmtBulk(ss, kv.second);
    }
    std::string stingtoWrite = ss.str();
    uint32_t secs = 1;
    if (stingtoWrite.size() > 1024*1024) {
        secs = 2;
    } else if (stingtoWrite.size() > 1024*1024*10) {
        secs = 4;
    }
    Status s = client->writeData(ss.str(), std::chrono::seconds(secs));
    if (!s.ok()) {
        return s;
    }
    Expected<std::string> exptOK = client->readLine(std::chrono::seconds(secs));
    if (!exptOK.ok()) {
        return exptOK.status();
    } else if (exptOK.value() != "+OK") {
        LOG(WARNING) << "store:" << storeId << " dst Store:" << dstStoreId
                     << " apply binlogs failed:" << exptOK.value();
        return {ErrorCodes::ERR_NETWORK, "bad return string"};
    }

    if (binlogs.size() == 0) {
        return binlogPos;
    } else {
        return binlogs[binlogs.size()-1].getReplLogKey().getTxnId();
    }
}

//  1) s->m INCRSYNC (m side: session2Client)
//  2) m->s +OK
//  3) s->m +PONG (s side: client2Session)
//  4) m->s periodly send binlogs
//  the 3) step is necessary, if ignored, the +OK in step 2) and binlogs
//  in step 4) may sticky together. and redis-resp protocal is not fixed-size
//  That makes client2Session complicated.

// NOTE(deyukong): we define binlogPos the greatest id that has been applied.
// "NOT" the smallest id that has not been applied. keep the same with BackupInfo's
// setCommitId
void ReplManager::registerIncrSync(asio::ip::tcp::socket sock,
            const std::string& storeIdArg,
            const std::string& dstStoreIdArg,
            const std::string& binlogPosArg) {
    std::shared_ptr<BlockingTcpClient> client =
        std::move(_svr->getNetwork()->createBlockingClient(
            std::move(sock), 64*1024*1024));

    uint64_t storeId;
    uint64_t  dstStoreId;
    uint64_t binlogPos;
    try {
        storeId = stoul(storeIdArg);
        dstStoreId = stoul(dstStoreIdArg);
        binlogPos = stoul(binlogPosArg);
    } catch (const std::exception& ex) {
        std::stringstream ss;
        ss << "-ERR parse opts failed:" << ex.what();
        client->writeLine(ss.str(), std::chrono::seconds(1));
        return;
    }

    if (storeId >= KVStore::INSTANCE_NUM ||
            dstStoreId >= KVStore::INSTANCE_NUM) {
        client->writeLine("-ERR invalid storeId", std::chrono::seconds(1));
        return;
    }

    uint64_t firstPos = [this, storeId]() {
        std::lock_guard<std::mutex> lk(_mutex);
        return _firstBinlogId[storeId];
    }();

    // NOTE(deyukong): this check is not precise
    // (not in the same critical area with the modification to _pushStatus),
    // but it does not harm correctness.
    // A strict check may be too complicated to read.
    if (firstPos > binlogPos) {
        client->writeLine("-ERR invalid binlogPos", std::chrono::seconds(1));
        return;
    }
    client->writeLine("+OK", std::chrono::seconds(1));
    Expected<std::string> exptPong = client->readLine(std::chrono::seconds(1));
    if (!exptPong.ok()) {
        LOG(WARNING) << "slave incrsync handshake failed:"
                << exptPong.status().toString();
        return;
    } else if (exptPong.value() != "+PONG") {
        LOG(WARNING) << "slave incrsync handshake not +PONG:"
                << exptPong.value();
        return;
    }

    std::string remoteHost = client->getRemoteRepr();
    bool registPosOk =
            [this,
             storeId,
             dstStoreId,
             binlogPos,
             client = std::move(client)]() mutable {
        std::lock_guard<std::mutex> lk(_mutex);
        if (_firstBinlogId[storeId] > binlogPos) {
            return false;
        }
        uint64_t clientId = _clientIdGen.fetch_add(1);
        _pushStatus[storeId][clientId] =
            std::move(std::unique_ptr<MPovStatus>(
                new MPovStatus {
                    isRunning: false,
                    dstStoreId: static_cast<uint32_t>(dstStoreId),
                    binlogPos: binlogPos,
                    nextSchedTime: SCLOCK::now(),
                    client: std::move(client),
                    clientId: clientId}));
        return true;
    }();
    LOG(INFO) << "slave:" << remoteHost
            << " registerIncrSync " << (registPosOk ? "ok" : "failed");
}

void ReplManager::supplyFullSyncRoutine(
            std::shared_ptr<BlockingTcpClient> client, uint32_t storeId) {
    PStore store = _svr->getSegmentMgr()->getInstanceById(storeId);
    INVARIANT(store != nullptr);
    if (!store->isRunning()) {
        client->writeLine("-ERR store is not running", std::chrono::seconds(1));
        return;
    }

    Expected<BackupInfo> bkInfo = store->backup();
    if (!bkInfo.ok()) {
        std::stringstream ss;
        ss << "-ERR backup failed:" << bkInfo.status().toString();
        client->writeLine(ss.str(), std::chrono::seconds(1));
        return;
    }

    auto guard = MakeGuard([this, store, storeId]() {
        Status s = store->releaseBackup();
        if (!s.ok()) {
            LOG(ERROR) << "supplyFullSync end clean store:"
                    << storeId << " error:" << s.toString();
        }
    });

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.StartObject();
    for (const auto& kv : bkInfo.value().getFileList()) {
        writer.Key(kv.first.c_str());
        writer.Uint64(kv.second);
    }
    writer.EndObject();
    Status s = client->writeLine(sb.GetString(), std::chrono::seconds(1));
    if (!s.ok()) {
        LOG(ERROR) << "store:" << storeId << " writeLine failed"
                    << s.toString();
        return;
    }

    std::string readBuf;
    readBuf.reserve(size_t(20ULL*1024*1024));  // 20MB
    for (auto& fileInfo : bkInfo.value().getFileList()) {
        s = client->writeLine(fileInfo.first, std::chrono::seconds(1));
        if (!s.ok()) {
            LOG(ERROR) << "write fname:" << fileInfo.first
                        << " to client failed:" << s.toString();
            return;
        }
        std::string fname = store->backupDir() + "/" + fileInfo.first;
        auto myfile = std::ifstream(fname, std::ios::binary);
        if (!myfile.is_open()) {
            LOG(ERROR) << "open file:" << fname << " for read failed";
            return;
        }
        size_t remain = fileInfo.second;
        while (remain) {
            size_t batchSize = std::min(remain, readBuf.capacity());
            readBuf.resize(batchSize);
            remain -= batchSize;
            myfile.read(&readBuf[0], batchSize);
            if (!myfile) {
                LOG(ERROR) << "read file:" << fname
                            << " failed with err:" << strerror(errno);
                return;
            }
            s = client->writeData(readBuf, std::chrono::seconds(1));
            if (!s.ok()) {
                LOG(ERROR) << "write bulk to client failed:" << s.toString();
                return;
            }
        }
    }
    Expected<std::string> reply = client->readLine(std::chrono::seconds(1));
    if (!reply.ok()) {
        LOG(ERROR) << "fullsync done read "
                   << client->getRemoteRepr() << " reply failed:"
                   << reply.status().toString();
    } else {
        LOG(INFO) << "fullsync done read "
                  << client->getRemoteRepr() << " reply:" << reply.value();
    }
}

}  // namespace tendisplus