/**
*    Copyright (C) 2013 Tokutek Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "mongo/pch.h"

#include "mongo/bson/bsonobjiterator.h"
#include "mongo/db/gtid.h"
#include "mongo/db/oplog.h"
#include "mongo/db/repl.h"
#include "mongo/db/txn_context.h"
#include "mongo/db/storage/env.h"
#include "mongo/util/stacktrace.h"

#include "mongo/s/d_logic.h"

#include "mongo/util/time_support.h"

namespace mongo {

    // making a static bool to tell us whether logging of operations is on
    // because this bool depends on replication, and replication is not
    // compiled with coredb. So, in startReplication, we will set this
    // to true
    static bool _logTxnOpsForReplication = false;
    static bool _logTxnOpsForSharding = false;
    static void (*_logTxnToOplog)(GTID gtid, uint64_t timestamp, uint64_t hash, BSONArray& opInfo) = NULL;
    static void (*_logTxnOpsRef)(GTID gtid, uint64_t timestamp, uint64_t hash, OID& oid) = NULL;
    static void (*_logOpsToOplogRef)(BSONObj o) = NULL;
    static bool (*_shouldLogOpForSharding)(const char *, const char *, const BSONObj &) = NULL;
    static bool (*_shouldLogUpdateOpForSharding)(const char *, const char *, const BSONObj &, const BSONObj &) = NULL;
    static void (*_startObjForMigrateLog)(BSONObjBuilder &b) = NULL;
    static void (*_writeObjToMigrateLog)(BSONObj &) = NULL;
    static void (*_writeObjToMigrateLogRef)(BSONObj &) = NULL;

    static GTIDManager* txnGTIDManager = NULL;

    TxnCompleteHooks *_completeHooks;

    void setLogTxnOpsForReplication(bool val) {
        _logTxnOpsForReplication = val;
    }

    bool logTxnOpsForReplication() {
        return _logTxnOpsForReplication;
    }

    void setLogTxnToOplog(void (*f)(GTID gtid, uint64_t timestamp, uint64_t hash, BSONArray& opInfo)) {
        _logTxnToOplog = f;
    }

    void setLogTxnRefToOplog(void (*f)(GTID gtid, uint64_t timestamp, uint64_t hash, OID& oid)) {
        _logTxnOpsRef = f;
    }

    void setLogOpsToOplogRef(void (*f)(BSONObj o)) {
        _logOpsToOplogRef = f;
    }

    void setTxnGTIDManager(GTIDManager* m) {
        txnGTIDManager = m;
    }

    void setTxnCompleteHooks(TxnCompleteHooks *hooks) {
        _completeHooks = hooks;
    }

    void enableLogTxnOpsForSharding(bool (*shouldLogOp)(const char *, const char *, const BSONObj &),
                                    bool (*shouldLogUpdateOp)(const char *, const char *, const BSONObj &, const BSONObj &),
                                    void (*startObj)(BSONObjBuilder &b),
                                    void (*writeObj)(BSONObj &),
                                    void (*writeObjToRef)(BSONObj &)) {
        _logTxnOpsForSharding = true;
        _shouldLogOpForSharding = shouldLogOp;
        _shouldLogUpdateOpForSharding = shouldLogUpdateOp;
        _startObjForMigrateLog = startObj;
        _writeObjToMigrateLog = writeObj;
        _writeObjToMigrateLogRef = writeObjToRef;
    }

    void disableLogTxnOpsForSharding(void) {
        _logTxnOpsForSharding = false;
        _shouldLogOpForSharding = NULL;
        _shouldLogUpdateOpForSharding = NULL;
        _startObjForMigrateLog = NULL;
        _writeObjToMigrateLog = NULL;
        _writeObjToMigrateLogRef = NULL;
    }

    bool logTxnOpsForSharding() {
        return _logTxnOpsForSharding;
    }

    bool shouldLogTxnOpForSharding(const char *opstr, const char *ns, const BSONObj &row) {
        if (!logTxnOpsForSharding()) {
            return false;
        }
        dassert(_shouldLogOpForSharding != NULL);
        return _shouldLogOpForSharding(opstr, ns, row);
    }

    bool shouldLogTxnUpdateOpForSharding(const char *opstr, const char *ns, const BSONObj &oldObj, const BSONObj &newObj) {
        if (!logTxnOpsForSharding()) {
            return false;
        }
        dassert(_shouldLogUpdateOpForSharding != NULL);
        return _shouldLogUpdateOpForSharding(opstr, ns, oldObj, newObj);
    }

    SpillableVector::SpillableVector(void (*writeObjToRef)(BSONObj &), size_t maxSize, SpillableVector *parent)
            : _writeObjToRef(writeObjToRef),
              _vec(),
              _curSize(0),
              _maxSize(maxSize),
              _parent(parent),
              _oid(_parent == NULL ? OID::gen() : _parent->_oid),
              _curObjInited(false),
              _buf(),
              _seq(0),
              _curSeqNo(_parent == NULL ? &_seq : _parent->_curSeqNo),
              _curObjBuilder(),
              _curArrayBuilder()
    {}

    void SpillableVector::append(const BSONObj &o) {
        BSONObj obj = o.getOwned();
        bool wasSpilling = spilling();
        _curSize += obj.objsize();
        if (!wasSpilling && spilling()) {
            spillAllObjects();
        }
        if (spilling()) {
            spillOneObject(obj);
        }
        else {
            _vec.push_back(obj.getOwned());
        }
    }

    void SpillableVector::getObjectsOrRef(BSONObjBuilder &b) {
        finish();
        dassert(_parent == NULL);
        if (spilling()) {
            b.append("refOID", _oid);
        }
        else {
            b.append("a", _vec);
        }
    }

    void SpillableVector::transfer() {
        finish();
        dassert(_parent != NULL);
        if (!spilling()) {
            // If your parent is spilling, or is about to start spilling, we'll take care of
            // spilling these in a moment.
            _parent->_vec.insert(_parent->_vec.end(), _vec.begin(), _vec.end());
        }
        _parent->_curSize += _curSize;
        if (_parent->spilling()) {
            // If your parent wasn't spilling before, this will spill their objects and yours in the
            // right order.  If they were already spilling, this will just spill your objects now.
            _parent->spillAllObjects();
        }
    }

    void SpillableVector::finish() {
        if (spilling()) {
            spillCurObj();
        }
    }

    void SpillableVector::initCurObj() {
        _curObjBuilder.reset(new BSONObjBuilder(_buf));
        _curObjBuilder->append("_id", BSON("oid" << _oid << "seq" << (*_curSeqNo)++));
        _curArrayBuilder.reset(new BSONArrayBuilder(_curObjBuilder->subarrayStart("a")));
    }

    void SpillableVector::spillCurObj() {
        if (_curArrayBuilder->arrSize() == 0) {
            return;
        }
        _curArrayBuilder->doneFast();
        BSONObj curObj = _curObjBuilder->done();
        _writeObjToRef(curObj);
    }

    void SpillableVector::spillOneObject(BSONObj obj) {
        if (!_curObjInited) {
            initCurObj();
            _curObjInited = true;
        }
        if (_curObjBuilder->len() + obj.objsize() >= (long long) _maxSize) {
            spillCurObj();
            _buf.reset();
            initCurObj();
        }
        _curArrayBuilder->append(obj);
    }

    void SpillableVector::spillAllObjects() {
        if (_parent != NULL) {
            // Your parent must spill anything they have before you do, to get the sequence numbers
            // right.
            _parent->spillAllObjects();
        }
        for (vector<BSONObj>::iterator it = _vec.begin(); it != _vec.end(); ++it) {
            spillOneObject(*it);
        }
        _vec.clear();
    }

    TxnContext::TxnContext(TxnContext *parent, int txnFlags)
            : _txn((parent == NULL) ? NULL : &parent->_txn, txnFlags), 
              _parent(parent),
              _retired(false),
              _txnOps(parent == NULL ? NULL : &parent->_txnOps),
              _txnOpsForSharding(_writeObjToMigrateLogRef,
                                 // transferMods has a maxSize of 1MB, we leave a few hundred bytes for metadata.
                                 1024 * 1024 - 512,
                                 parent == NULL ? NULL : &parent->_txnOpsForSharding),
              _initiatingRS(false)
    {
    }

    TxnContext::~TxnContext() {
        if (!_retired) {
            abort();
        }
    }

    void TxnContext::commit(int flags) {
        bool gotGTID = false;
        GTID gtid;
        // do this in case we are writing the first entry
        // we put something in that can be distinguished from
        // an initialized GTID that has never been touched
        gtid.inc_primary(); 
        // handle work related to logging of transaction for replication
        // this piece must be done before the _txn.commit
        try {
            if (hasParent()) {
                // This does something
                // a bit dangerous in that it may spill parent's stuff
                // with this child transaction that is committing. If something
                // goes wrong and this child transaction aborts, we will miss
                // some ops
                //
                // This ought to be ok, because we are in this try/catch block
                // where if something goes wrong, we will crash the server.
                // NOTHING better go wrong here, unless under bad rare
                // circumstances
                _txnOps.finishChildCommit();
            }
            else if (!_txnOps.empty()) {
                uint64_t timestamp = 0;
                uint64_t hash = 0;
                if (!_initiatingRS) {
                    dassert(txnGTIDManager);
                    txnGTIDManager->getGTIDForPrimary(&gtid, &timestamp, &hash);
                }
                else {
                    dassert(!txnGTIDManager);
                    timestamp = curTimeMillis64();
                }
                gotGTID = true;
                // In this case, the transaction we are committing has
                // no parent, so we must write the transaction's 
                // logged operations to the opLog, as part of this transaction
                dassert(logTxnOpsForReplication());
                dassert(_logTxnToOplog);
                _txnOps.rootCommit(gtid, timestamp, hash);
            }
            // handle work related to logging of transaction for chunk migrations
            if (!_txnOpsForSharding.empty()) {
                if (hasParent()) {
                    transferOpsForShardingToParent();
                }
                else {
                    writeTxnOpsToMigrateLog();
                }
            }

            _clientCursorRollback.preComplete();
            _txn.commit(flags);

            // if the commit of this transaction got a GTID, then notify 
            // the GTIDManager that the commit is now done.
            if (gotGTID && !_initiatingRS) {
                dassert(txnGTIDManager);
                // save the GTID for the client so that
                // getLastError will know what GTID slaves
                // need to be caught up to.
                cc().setLastOp(gtid);
                txnGTIDManager->noteLiveGTIDDone(gtid);
            }
        }
        catch (std::exception &e) {
            log() << "exception during critical section of txn commit, aborting system: " << e.what() << endl;
            printStackTrace();
            logflush();
            ::abort();
        }

        // These rollback items must be processed after the ydb transaction completes.
        if (hasParent()) {
            _cappedRollback.transfer(_parent->_cappedRollback);
            _nsIndexRollback.transfer(_parent->_nsIndexRollback);
        } else {
            _cappedRollback.commit();
            _nsIndexRollback.commit();
        }
        _retired = true;
    }

    void TxnContext::abort() {
        _clientCursorRollback.preComplete();
        _nsIndexRollback.preAbort();
        _txnOps.abort();
        _txn.abort();
        _cappedRollback.abort();
        _retired = true;
    }

    void TxnContext::logOpForReplication(BSONObj op) {
        dassert(logTxnOpsForReplication());
        _txnOps.appendOp(op);
    }

    void TxnContext::logOpForSharding(BSONObj op) {
        dassert(logTxnOpsForSharding());
        _txnOpsForSharding.append(op);
    }

    bool TxnContext::hasParent() {
        return (_parent != NULL);
    }

    void TxnContext::txnIntiatingRs() {
        _initiatingRS = true;
    }

    void TxnContext::transferOpsForShardingToParent() {
        _txnOpsForSharding.transfer();
    }

    void TxnContext::writeTxnOpsToMigrateLog() {
        dassert(logTxnOpsForSharding());
        dassert(_startObjForMigrateLog != NULL);
        dassert(_writeObjToMigrateLog != NULL);
        BSONObjBuilder b;
        _startObjForMigrateLog(b);
        _txnOpsForSharding.getObjectsOrRef(b);
        BSONObj obj = b.done();
        _writeObjToMigrateLog(obj);
    }

    /* --------------------------------------------------------------------- */

    void CappedCollectionRollback::_complete(const bool committed) {
        for (ContextMap::const_iterator it = _map.begin(); it != _map.end(); it++) {
            const string &ns = it->first;
            const Context &c = it->second;
            _completeHooks->noteTxnCompletedInserts(ns, c.minPK, c.nDelta, c.sizeDelta, committed);
        }
    }

    void CappedCollectionRollback::commit() {
        _complete(true);
    }

    void CappedCollectionRollback::abort() {
        _complete(false);
    }

    void CappedCollectionRollback::transfer(CappedCollectionRollback &parent) {
        for (ContextMap::const_iterator it = _map.begin(); it != _map.end(); it++) {
            const string &ns = it->first;
            const Context &c = it->second;
            Context &parentContext = parent._map[ns];
            if (parentContext.minPK.isEmpty()) {
                parentContext.minPK = c.minPK;
            } else if (!c.minPK.isEmpty()) {
                dassert(parentContext.minPK <= c.minPK);
            }
            parentContext.nDelta += c.nDelta;
            parentContext.sizeDelta += c.sizeDelta;
        }
    }

    void CappedCollectionRollback::noteInsert(const string &ns, const BSONObj &pk, long long size) {
        Context &c = _map[ns];
        if (c.minPK.isEmpty()) {
            c.minPK = pk.getOwned();
        }
        dassert(c.minPK <= pk);
        c.nDelta++;
        c.sizeDelta += size;
    }

    void CappedCollectionRollback::noteDelete(const string &ns, const BSONObj &pk, long long size) {
        Context &c = _map[ns];
        c.nDelta--;
        c.sizeDelta -= size;
    }

    bool CappedCollectionRollback::hasNotedInsert(const string &ns) {
        const Context &c = _map[ns];
        return !c.minPK.isEmpty();
    }

    /* --------------------------------------------------------------------- */

    void NamespaceIndexRollback::commit() {
        // nothing to do on commit
    }

    void NamespaceIndexRollback::preAbort() {
        _completeHooks->noteTxnAbortedFileOps(_namespaces, _dbs);
    }

    void NamespaceIndexRollback::transfer(NamespaceIndexRollback &parent) {
        TOKULOG(1) << "NamespaceIndexRollback::transfer processing "
                   << _namespaces.size() + _dbs.size() << " roll items." << endl;

        // Promote rollback entries to parent.
        parent._namespaces.insert(_namespaces.begin(), _namespaces.end());
        parent._dbs.insert(_dbs.begin(), _dbs.end());
    }

    void NamespaceIndexRollback::noteNs(const char *ns) {
        _namespaces.insert(ns);
    }

    void NamespaceIndexRollback::noteCreate(const string &dbname) {
        _dbs.insert(dbname);
    }

    /* --------------------------------------------------------------------- */

    void ClientCursorRollback::preComplete() {
        _completeHooks->noteTxnCompletedCursors(_cursorIds);
    }

    void ClientCursorRollback::noteClientCursor(long long id) {
        _cursorIds.insert(id);
    }

    TxnOplog::TxnOplog(TxnOplog *parent) : _parent(parent), _spilled(false), _mem_size(0), _mem_limit(cmdLine.txnMemLimit) {
        // This is initialized to 1 so that the query in applyRefOp in
        // oplog.cpp can
        _seq = 1;
        if (_parent) { // child inherits the parents seq number and spilled state
            _seq = _parent->_seq + 1;
        }
    }

    TxnOplog::~TxnOplog() {
    }

    void TxnOplog::appendOp(BSONObj o) {
        _seq++;
        _m.push_back(o);
        _mem_size += o.objsize();
        if (_mem_size > _mem_limit) {
            spill();
            _spilled = true;
        }
    }

    bool TxnOplog::empty() const {
        return !_spilled && (_m.size() == 0);
    }

    void TxnOplog::spill() {
        // it is possible to have spill called when there
        // is nothing to actually spill. For instance, when
        // the root commits and we have already spilled,
        // we call spill to write out any remaining ops, of which
        // there may be none
        if (_m.size() > 0) {
            if (!_oid.isSet()) {
                _oid = getOid();
            }
            BSONObjBuilder b;

            // build the _id
            BSONObjBuilder b_id;
            b_id.append("oid", _oid);
            // probably not necessary to increment _seq, but safe to do
            b_id.append("seq", ++_seq);
            b.append("_id", b_id.obj());

            // build the ops array
            BSONArrayBuilder b_a;
            while (_m.size() > 0) {
                BSONObj o = _m.front();
                b_a.append(o);
                _m.pop_front();
                _mem_size -= o.objsize();
            }
            b.append("ops", b_a.arr());

            verify(_m.size() == 0);
            verify(_mem_size == 0);

            // insert it
            dassert(_logOpsToOplogRef);
            _logOpsToOplogRef(b.obj());
        }
        else {
            // just a sanity check
            verify(_oid.isSet());
        }
    }

    OID TxnOplog::getOid() {
        if (!_oid.isSet()) {
            if (!_parent) {
                _oid = OID::gen();
            } else {
                _oid = _parent->getOid();
            }
        }
        return _oid;
    }

    void TxnOplog::writeOpsDirectlyToOplog(GTID gtid, uint64_t timestamp, uint64_t hash) {
        dassert(logTxnOpsForReplication());
        dassert(_logTxnToOplog);
        // build array of in memory ops
        BSONArrayBuilder b;
        for (deque<BSONObj>::iterator it = _m.begin(); it != _m.end(); it++) {
            b.append(*it);
        }
        BSONArray a = b.arr();
        // log ops
        _logTxnToOplog(gtid, timestamp, hash, a);
    }

    void TxnOplog::writeTxnRefToOplog(GTID gtid, uint64_t timestamp, uint64_t hash) {
        dassert(logTxnOpsForReplication());
        dassert(_logTxnOpsRef);
        // log ref
        _logTxnOpsRef(gtid, timestamp, hash, _oid);
    }

    void TxnOplog::rootCommit(GTID gtid, uint64_t timestamp, uint64_t hash) {
        if (_spilled) {
            // spill in memory ops if any
            spill();
            // log ref
            writeTxnRefToOplog(gtid, timestamp, hash);
        } else {
            writeOpsDirectlyToOplog(gtid, timestamp, hash);
        }
    }

    void TxnOplog::finishChildCommit() {
        // parent inherits the childs seq number and spilled state
        verify(_seq > _parent->_seq);
        // the first thing we want to do, is if child has spilled,
        // we must get the parent to spill.
        // The parent will have a _seq that is smaller than
        // any seq we used, so the data that is spilled will be
        // correctly positioned behind all of the work we have done
        // in the oplog.refs collection. For that reason, this must be
        // done BEFORE we set the parent's _seq to our _seq + 1
        if (_spilled) {
            _parent->spill();
            _parent->_spilled = _spilled;
        }
        _parent->_seq = _seq+1;
        // move to parent
        for (deque<BSONObj>::iterator it = _m.begin(); it != _m.end(); it++) {
            _parent->appendOp(*it);
        }
    }

    void TxnOplog::abort() {
        // nothing to do on abort
    }

} // namespace mongo
