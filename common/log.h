// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * log.h:
 *   a replica's log of pending and committed operations
 *
 * Copyright 2013 Dan R. K. Ports  <drkp@cs.washington.edu>
 *		  Jialin Li	   <lijl@cs.washington.edu>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/

#ifndef _COMMON_LOG_H_
#define _COMMON_LOG_H_

#include "common/request.pb.h"
#include "lib/assert.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "lib/viewstamp.h"

#include <map>
#include <google/protobuf/message.h>
#include <openssl/sha.h>

namespace specpaxos {

enum LogEntryState {
    LOG_STATE_COMMITTED,
    LOG_STATE_PREPARED,
    LOG_STATE_SPECULATIVE,      // specpaxos only
    LOG_STATE_FASTPREPARED,     // fastpaxos only
    LOG_STATE_RECEIVED,		// nopaxos only
    LOG_STATE_NOOP,		// nopaxos only
    LOG_STATE_EXECUTED          // granola
};


template <typename D>
class Log
{

public:
    struct LogEntry
    {
        viewstamp_t viewstamp;
        LogEntryState state;
        Request request;
        string hash;
        // Speculative client table stuff
        opnum_t prevClientReqOpnum;
        ::google::protobuf::Message *replyMessage;
        // Other data defined by protocols
        D data;

        LogEntry() { replyMessage = NULL;}
        LogEntry(const LogEntry &x)
            : viewstamp(x.viewstamp), state(x.state), request(x.request),
              hash(x.hash), prevClientReqOpnum(x.prevClientReqOpnum)
            {
                if (x.replyMessage) {
                    replyMessage = x.replyMessage->New();
                    replyMessage->CopyFrom(*x.replyMessage);
                } else {
                    replyMessage = NULL;
                }
            }
        LogEntry(viewstamp_t viewstamp,
                 LogEntryState state,
                 const Request &request,
                 const string &hash=Log::EMPTY_HASH)
            : viewstamp(viewstamp), state(state), request(request),
              hash(hash), replyMessage(NULL) { }
        virtual ~LogEntry()
            {
                if (replyMessage) {
                    delete replyMessage;
                }
            }
    };

    Log(bool useHash, opnum_t start = 1, string initialHash = EMPTY_HASH);
    LogEntry & Append(viewstamp_t vs, const Request &req, LogEntryState state);
    LogEntry & Append(viewstamp_t vs, const Request &req, LogEntryState state,
                      const D &data);
    LogEntry & Append(viewstamp_t vs, const Request &req,
                      const multistamp_t &stamp,
                      LogEntryState state,
                      const D &data);
    LogEntry * Find(opnum_t opnum);
    LogEntry * Find(viewstamp_t vs);
    LogEntry * Find(const std::pair<uint64_t, uint64_t> &reqid);
    bool SetStatus(opnum_t opnum, LogEntryState state);
    bool SetRequest(opnum_t op, const Request &req);
    void RemoveAfter(opnum_t opnum);
    LogEntry * Last();
    viewstamp_t LastViewstamp() const; // deprecated
    opnum_t LastOpnum() const;
    opnum_t FirstOpnum() const;
    bool Empty() const;
    template <class T> void Dump(opnum_t from, T out);
    template <class T> void Dump(opnum_t from, opnum_t to, T out);
    template <class iter> void Install(iter start, iter end);
    const string &LastHash() const;

    static string ComputeHash(string lastHash, const LogEntry &entry);
    static const string EMPTY_HASH;


private:
    std::vector<LogEntry> entries;
    string initialHash;
    opnum_t start;
    bool useHash;

    // Eris: Search log entry using viewstamps (from other shards)
    std::map<viewstamp_t, opnum_t> vssMap;
    // Granola: Search log entry using <clientid, clientreqid> tuple
    std::map<std::pair<uint64_t, uint64_t>, opnum_t> clientReqMap;
};

#include "common/log-impl.h"

template <typename D>
const string Log<D>::EMPTY_HASH = string(SHA_DIGEST_LENGTH, '\0');

template <typename D>
Log<D>::Log(bool useHash, opnum_t start, string initialHash)
    : useHash(useHash)
{
    this->initialHash = initialHash;
    this->start = start;
    if (start == 1) {
        ASSERT(initialHash == EMPTY_HASH);
    }

    // reserve enough space to reduce reallocation overhead
    entries.reserve(10000000);
}

template <typename D>
typename Log<D>::LogEntry &
Log<D>::Append(viewstamp_t vs, const Request &req, LogEntryState state)
{
    D data;
    return Append(vs, req, state, data);
}

template <typename D>
typename Log<D>::LogEntry &
Log<D>::Append(viewstamp_t vs, const Request &req, LogEntryState state, const D &data)
{
    if (entries.empty()) {
        ASSERT(vs.opnum == start);
    } else {
        ASSERT(vs.opnum == LastOpnum()+1);
    }

    string prevHash = LastHash();
    entries.push_back(LogEntry(vs, state, req));
    entries.back().data = data;
    if (useHash) {
        entries.back().hash = ComputeHash(prevHash, entries.back());
    }

    if (req.clientid() > 0 && req.clientreqid() > 0) {
        this->clientReqMap[std::make_pair(req.clientid(), req.clientreqid())] = vs.opnum;
    }

    return entries.back();
}

template <typename D>
typename Log<D>::LogEntry &
Log<D>::Append(viewstamp_t vs,
               const Request &req,
               const multistamp_t &stamp,
               LogEntryState state,
               const D &data)
{
    for (const auto &kv : stamp.seqnums) {
        vssMap[viewstamp_t(0, 0, stamp.sessnum, kv.second, kv.first)] = vs.opnum;
    }
    return Append(vs, req, state, data);
}

// This really ought to be const
template <typename D>
typename Log<D>::LogEntry *
Log<D>::Find(opnum_t opnum)
{
    if (entries.empty()) {
        return NULL;
    }

    if (opnum < start) {
        return NULL;
    }

    if (opnum-start > entries.size()-1) {
        return NULL;
    }

    LogEntry *entry = &entries[opnum-start];
    ASSERT(entry->viewstamp.opnum == opnum);
    return entry;
}

template <typename D>
typename Log<D>::LogEntry *
Log<D>::Find(viewstamp_t vs)
{
    if (vssMap.find(vs) == vssMap.end()) {
        return NULL;
    }

    return Find(vssMap[vs]);
}

template <typename D>
typename Log<D>::LogEntry *
Log<D>::Find(const std::pair<uint64_t, uint64_t> &reqid)
{
    if (clientReqMap.find(reqid) == clientReqMap.end()) {
        return NULL;
    }

    return Find(clientReqMap[reqid]);
}

template <typename D>
bool
Log<D>::SetStatus(opnum_t op, LogEntryState state)
{
    LogEntry *entry = Find(op);
    if (entry == NULL) {
        return false;
    }

    entry->state = state;
    return true;
}

template <typename D>
bool
Log<D>::SetRequest(opnum_t op, const Request &req)
{
    if (useHash) {
        Panic("Log::SetRequest on hashed log not supported.");
    }

    LogEntry *entry = Find(op);
    if (entry == NULL) {
        return false;
    }

    entry->request = req;
    return true;
}

template <typename D>
void
Log<D>::RemoveAfter(opnum_t op)
{
#if PARANOID
    // We'd better not be removing any committed entries.
    for (opnum_t i = op; i <= LastOpnum(); i++) {
        ASSERT(Find(i)->state != LOG_STATE_COMMITTED);
    }
#endif

    if (op > LastOpnum()) {
        return;
    }

    Debug("Removing log entries after " FMT_OPNUM, op);

    ASSERT(op-start < entries.size());
    entries.resize(op-start);

    ASSERT(LastOpnum() == op-1);
}

template <typename D>
typename Log<D>::LogEntry *
Log<D>::Last()
{
    if (entries.empty()) {
        return NULL;
    }

    return &entries.back();
}

template <typename D>
viewstamp_t
Log<D>::LastViewstamp() const
{
    if (entries.empty()) {
        return viewstamp_t(0, start-1);
    } else {
        return entries.back().viewstamp;
    }
}

template <typename D>
opnum_t
Log<D>::LastOpnum() const
{
    if (entries.empty()) {
        return start-1;
    } else {
        return entries.back().viewstamp.opnum;
    }
}

template <typename D>
opnum_t
Log<D>::FirstOpnum() const
{
    // XXX Not really sure what's appropriate to return here if the
    // log is empty
    return start;
}

template <typename D>
bool
Log<D>::Empty() const
{
    return entries.empty();
}

template <typename D>
const string &
Log<D>::LastHash() const
{
    if (entries.empty()) {
        return initialHash;
    } else {
        return entries.back().hash;
    }
}

template <typename D>
string
Log<D>::ComputeHash(string lastHash, const LogEntry &entry)
{
    SHA_CTX ctx;
    unsigned char out[SHA_DIGEST_LENGTH];

    SHA1_Init(&ctx);

    SHA1_Update(&ctx, lastHash.c_str(), lastHash.size());
    //SHA1_Update(&ctx, &entry.viewstamp, sizeof(entry.viewstamp));
    uint64_t x[2];
    x[0] = entry.request.clientid();
    x[1] = entry.request.clientreqid();
    SHA1_Update(&ctx, x, sizeof(uint64_t)*2);
    // SHA1_Update(&ctx, entry.request.op().c_str(),
    //             entry.request.op().size());

    SHA1_Final(out, &ctx);

    return string((char *)out, SHA_DIGEST_LENGTH);
}

}      // namespace specpaxos

#endif  /* _COMMON_LOG_H_ */
