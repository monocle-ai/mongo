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

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/base/init.h"
#include "mongo/db/collection.h"
#include "mongo/db/cursor.h"
#include "mongo/db/database.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/index.h"
#include "mongo/db/index_set.h"
#include "mongo/db/oplog_helpers.h"
#include "mongo/db/relock.h"
#include "mongo/db/txn_context.h"
#include "mongo/db/querypattern.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/query_plan_selection_policy.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/storage/key.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/scripting/engine.h"

namespace mongo {

    static void removeFromNamespacesCatalog(const StringData &ns);
    static void removeFromIndexesCatalog(const StringData &ns, const StringData &name);

    CollectionMap *collectionMap(const StringData &ns) {
        Database *database = cc().database();
        verify(database);
        DEV {
            StringData db = nsToDatabaseSubstring(ns);
            if (db != database->name()) {
                out() << "ERROR: attempt to write to wrong database\n";
                out() << " ns:" << ns << '\n';
                out() << " database->name:" << database->name() << endl;
                verify(db == database->name());
            }
        }
        return &database->_collectionMap;
    }

    Collection *getCollection(const StringData& ns) {
        return collectionMap(ns)->getCollection(ns);
    }

    // Internal getOrCreate: Does not run the create command.
    Collection *_getOrCreateCollection(const StringData &ns, const BSONObj options = BSONObj()) {
        CollectionMap *cm = collectionMap(ns);
        if (!cm->allocated()) {
            // Must make sure we loaded any existing namespaces before checking, or we might create one that already exists.
            cm->init(true);
        }
        Collection *cl = cm->getCollection(ns);
        if (cl == NULL) {
            TOKULOG(2) << "Didn't find ns " << ns << ", creating it." << endl;
            if (!Lock::isWriteLocked(ns)) {
                throw RetryWithWriteLock();
            }

            shared_ptr<Collection> newCollection(Collection::make(ns, options));
            cm->add_ns(ns, newCollection);

            cl = cm->getCollection(ns);
            cl->addDefaultIndexesToCatalog();

            // Keep the call to 'str()', it allows us to call it in gdb.
            TOKULOG(2) << "Created collection " << options.str() << endl;
        }
        return cl;
    }

    // External getOrCreate: runs the "create" command if necessary.
    Collection *getOrCreateCollection(const StringData& ns, bool logop) {
        Collection *cl = getCollection(ns);
        if (cl == NULL) {
            string err;
            BSONObj options;
            bool created = userCreateNS(ns, options, err, logop);
            uassert(16745, "failed to create collection", created);
            cl = getCollection(ns);
            uassert(16746, "failed to get collection after creating", cl);
        }
        return cl;
    }

    /* ------------------------------------------------------------------------- */

    BSONObj CollectionBase::indexInfo(const BSONObj &keyPattern, bool unique, bool clustering, BSONObj options) const {
        BSONObjBuilder b;
        b.append("ns", _ns);
        b.append("key", keyPattern);
        if (keyPattern == BSON("_id" << 1)) {
            b.append("name", "_id_");
        } else if (keyPattern == BSON("$_" << 1)) {
            b.append("name", "$_");
        } else {
            b.append("name", "primaryKey");
        }
        if (unique) {
            b.appendBool("unique", true);
        }
        if (clustering) {
            b.appendBool("clustering", true);
        }

        BSONElement e;
        e = options["readPageSize"];
        if (e.ok() && !e.isNull()) {
            b.append(e);
        }
        e = options["pageSize"];
        if (e.ok() && !e.isNull()) {
            b.append(e);
        }
        e = options["compression"];
        if (e.ok() && !e.isNull()) {
            b.append(e);
        }
        e = _options["fanout"];
        if (e.ok() && !e.isNull()) {
            b.append(e);
        }
        return b.obj();
    }

    static bool isSystemCatalog(const StringData &ns) {
        StringData coll = nsToCollectionSubstring(ns);
        return coll == "system.indexes" || coll == "system.namespaces";
    }

    static bool isProfileCollection(const StringData &ns) {
        StringData coll = nsToCollectionSubstring(ns);
        return coll == "system.profile";
    }

    static bool isOplogCollection(const StringData &ns) {
        return ns == rsoplog;
    }

    // Instantiate the common information about a collection
    Collection::Collection(const StringData &ns, const BSONObj &options) :
        _ns(ns.toString()),
        _options(options.copy()) {

        massert(10356, str::stream() << "invalid ns: " << ns,
                       NamespaceString::validCollectionName(ns));

        // TODO: do some stuff to create _cd
        if (isOplogCollection(ns)) {
            if (options["partitioned"].trueValue()) {
                PartitionedOplogCollection* poc = new PartitionedOplogCollection(ns, options);
                poc->initialize(ns, options);
                _cd.reset(poc);
            }
            else {
                _cd.reset(new OldOplogCollection(ns, options));
            }
        } else if (isSystemCatalog(ns)) {
            _cd.reset(new SystemCatalogCollection(ns, options));
        } else if (isSystemUsersCollection(ns)) {
            Client::CreatingSystemUsersScope scope;
            _cd.reset(new SystemUsersCollection(ns, options));
        } else if (isProfileCollection(ns)) {
            // TokuMX doesn't _necessarily_ need the profile to be capped, but vanilla does.
            // We enforce the restriction because it's easier to implement. See SERVER-6937.
            uassert( 16852, "System profile must be a capped collection.", options["capped"].trueValue() );
            _cd.reset(new ProfileCollection(ns, options));
        } else if (options["partitioned"].trueValue()) {
            uassert(0, "Partitioned Collection cannot be capped", !options["capped"].trueValue());
            PartitionedCollection* pc = new PartitionedCollection(ns, options);
            pc->initialize(ns, options);
            _cd.reset(pc);
        } else if (options["capped"].trueValue()) {
            _cd.reset(new CappedCollection(ns, options));
        } else if (options["natural"].trueValue()) {
            _cd.reset(new NaturalOrderCollection(ns, options));
        } else {
            _cd.reset(new IndexedCollection(ns, options));
        }

        // after _cd stuff:
        _pk = _cd->pkPattern().copy();
        collectionMap(_ns)->update_ns(_ns, serialize(true), true);
        try {
            // If this throws, it's safe to call close() because we just created the index.
            // Therefore we have a write lock, and nobody else could have any uncommitted
            // modifications to this index, so close() should succeed, and #29 is irrelevant.
            addToNamespacesCatalog(ns, !options.isEmpty() ? &options : NULL);
        } catch (...) {
            close();
            throw;
        }
        computeIndexKeys();
    }

    // Construct an existing collection given its serialized from (generated via serialize()).
    Collection::Collection(const BSONObj &serialized, bool bulkLoad) :
        _ns(serialized["ns"].String()),
        _options(serialized["options"].Obj().copy()) {

        const StringData ns = serialized["ns"].Stringdata();
        bool reserializeNeeded = false;
        if (isOplogCollection(ns)) {
            // We may bulk load the oplog since it's an IndexedCollection
            if (bulkLoad) {
                uassert( 0, "Should not bulk load the oplog", !bulkLoad );
            }
            if (serialized["options"]["partitioned"].trueValue()) {
                PartitionedOplogCollection* poc = new PartitionedOplogCollection(serialized);
                poc->initialize(serialized);
                _cd.reset(poc);
            }
            else {
                _cd.reset(new OldOplogCollection(serialized));
            }                              
        } else if (isSystemCatalog(ns)) {
            massert( 16869, "bug: Should not bulk load a system catalog collection", !bulkLoad );
            _cd.reset(new SystemCatalogCollection(serialized));
        } else if (isSystemUsersCollection(ns)) {
            massert( 17002, "bug: Should not bulk load the users collection", !bulkLoad );
            Client::CreatingSystemUsersScope scope;
            _cd.reset(new SystemUsersCollection(serialized, &reserializeNeeded));

            int idx = _cd->findIndexByKeyPattern(extendedSystemUsersKeyPattern);
            if (idx < 0) {
                BSONObj info = SystemUsersCollection::extendedSystemUsersIndexInfo(_ns);
                _cd->ensureIndex(info);
                addToIndexesCatalog(info);
                reserializeNeeded = true;
            }
            idx = findIndexByKeyPattern(oldSystemUsersKeyPattern);
            if (idx >= 0) {
                // Just to get compile going
                dropIndex(idx);
                reserializeNeeded = true;
            }
        } else if (isProfileCollection(ns)) {
            massert( 16870, "bug: Should not bulk load the profile collection", !bulkLoad );
            _cd.reset(new ProfileCollection(serialized));
        } else if (serialized["options"]["partitioned"].trueValue()) {
            massert( 17247, "bug: Should not bulk load partitioned collections", !bulkLoad );
            PartitionedCollection* pc = new PartitionedCollection(serialized);
            pc->initialize(serialized);
            _cd.reset(pc);
        } else if (serialized["options"]["capped"].trueValue()) {
            massert( 16871, "bug: Should not bulk load capped collections", !bulkLoad );
            _cd.reset(new CappedCollection(serialized));
        } else if (serialized["options"]["natural"].trueValue()) {
            massert( 16872, "bug: Should not bulk load natural order collections. ", !bulkLoad );
            _cd.reset(new NaturalOrderCollection(serialized));
        } else {
            // We only know how to bulk load indexed collections.
            if (bulkLoad) {
                _cd.reset(new BulkLoadedCollection(serialized));
            }
            else {
                _cd.reset(new IndexedCollection(serialized));
            }                              
        }
        if (reserializeNeeded) {
            // Write a clean version of this collection's info to the collection map, now that we've rectified it.
            collectionMap(_ns)->update_ns(_ns, serialize(), true);
        }
        _pk = _cd->pkPattern().copy();

        // after _cd stuff:
        computeIndexKeys();
    }

    Collection::~Collection() { }

    // Used by index.cpp for system.users upgrade detection
    bool isSystemUsersCollection(const StringData &ns) {
        StringData coll = nsToCollectionSubstring(ns);
        return coll == "system.users";
    }



    // Construct a brand new Collection with a certain primary key and set of options.
    //
    // Factories for making an appropriate subtype of Collection
    //

    shared_ptr<Collection> Collection::make(const StringData &ns, const BSONObj &options) {
    /*
        */
        shared_ptr<Collection> ret;
        ret.reset(new Collection(ns, options));
        return ret;
    }

    shared_ptr<Collection> Collection::make(const BSONObj &serialized, const bool bulkLoad) {
        shared_ptr<Collection> ret;
        ret.reset(new Collection(serialized, bulkLoad));
        return ret;
    }

    void Collection::resetTransient() {
        Lock::assertWriteLocked(_ns); 
        _queryCache.clearQueryCache();
        computeIndexKeys();
    }

    void Collection::computeIndexKeys() {
        _indexedPaths.clear();

        for (int i = 0; i < nIndexesBeingBuilt(); i++) {
            const BSONObj &key = idx(i).keyPattern();
            BSONObjIterator o( key );
            while ( o.more() ) {
                const BSONElement e = o.next();
                _indexedPaths.addPath( e.fieldName() );
            }
        }
    }

    bool Collection::findOne(const StringData &ns, const BSONObj &query,
                             BSONObj &result, const bool requireIndex) {
        for (shared_ptr<Cursor> c(getOptimizedCursor(ns, query, BSONObj(),
                                      requireIndex ? QueryPlanSelectionPolicy::indexOnly() :
                                                     QueryPlanSelectionPolicy::any()));
             c->ok(); c->advance()) {
            if (c->currentMatches() && !c->getsetdup(c->currPK())) {
                result = c->current().getOwned();
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------------

    CollectionBase::CollectionBase(const StringData &ns, const BSONObj &pkIndexPattern, const BSONObj &options) :
        CollectionData(ns, pkIndexPattern),
        _indexBuildInProgress(false),
        _nIndexes(0),
        _fastupdatesOkState(AtomicWord<int>(-1)),
        _multiKeyIndexBits(0) {

        TOKULOG(1) << "Creating collection " << ns << endl;

        // Create the primary key index, generating the info from the pk pattern and options.
        BSONObj info = indexInfo(pkIndexPattern, true, true, options);
        createIndex(info);
    }

    // Construct an existing collection given its serialized from (generated via serialize()).
    CollectionBase::CollectionBase(const BSONObj &serialized, bool* reserializeNeeded) :
        CollectionData(serialized),
        _indexBuildInProgress(false),
        _nIndexes(serialized["indexes"].Array().size()),
        _fastupdatesOkState(AtomicWord<int>(-1)),
        _multiKeyIndexBits(static_cast<uint64_t>(serialized["multiKeyIndexBits"].Long())){

        bool reserialize = false;
        std::vector<BSONElement> index_array = serialized["indexes"].Array();
        // TODO: Find out why this code is in this constructor and not the SystemUsersCollection constructor
        for (std::vector<BSONElement>::iterator it = index_array.begin(); it != index_array.end(); it++) {
            const BSONObj &info = it->Obj();
            shared_ptr<IndexDetailsBase> idx(IndexDetailsBase::make(info, false));
            if (!idx && cc().upgradingSystemUsers() && isSystemUsersCollection(_ns) &&
                oldSystemUsersKeyPattern == info["key"].Obj()) {
                // This was already dropped, but because of #673 we held on to the info.
                // To fix it, just drop the index info on the floor.
                LOG(0) << "Incomplete upgrade of " << _ns << " indexes detected.  Repairing." << endl;
                reserialize = true;
                size_t idxNum = it - index_array.begin();
                // Removes the nth bit, and shifts any bits higher than it down a slot.
                _multiKeyIndexBits = ((_multiKeyIndexBits & ((1ULL << idxNum) - 1)) |
                                      ((_multiKeyIndexBits >> (idxNum + 1)) << idxNum));
                _nIndexes--;
                continue;
            }
            _indexes.push_back(idx);
        }
        if (reserialize) {
            // only the system users collection should possibly need
            // reserializing, and that is the only constructor that pipes up this parameter
            verify(reserializeNeeded != NULL);
            *reserializeNeeded = true;
        }
    }

    // Serialize the information necessary to re-open this collection later.
    BSONObj Collection::serialize(const StringData& ns, const BSONObj &options, const BSONObj &pk,
                                      unsigned long long multiKeyIndexBits, const BSONArray &indexes_array) {
        return BSON("ns" << ns <<
                    "options" << options <<
                    "pk" << pk <<
                    "multiKeyIndexBits" << static_cast<long long>(multiKeyIndexBits) <<
                    "indexes" << indexes_array);
    }

    BSONObj Collection::serialize(const bool includeHotIndex) const {
        BSONArrayBuilder indexes_array;
        // Serialize all indexes that exist, including a hot index if it exists.
        for (int i = 0; i < (includeHotIndex ? nIndexesBeingBuilt() : nIndexes()); i++) {
            IndexDetails &currIdx = idx(i);
            indexes_array.append(currIdx.info());
        }
        return serialize(_ns, _options, _pk, _cd->getMultiKeyIndexBits(), indexes_array.arr());
    }

    void Collection::noteMultiKeyChanged() {
        collectionMap(_ns)->update_ns(_ns, serialize(), true);
        resetTransient();
    }


    void CollectionBase::close(const bool aborting, bool* indexBitsChanged) {
        *indexBitsChanged = false;
        if (!aborting) {
            verify(!_indexBuildInProgress);
        }
        for (int i = 0; i < nIndexesBeingBuilt(); i++) {
            IndexDetailsBase &idx = *_indexes[i];
            idx.close();
        }
    }

    bool CollectionBase::fastupdatesOk() {
        const int state = _fastupdatesOkState.loadRelaxed();
        if (state == -1) {
            // need to determine if fastupdates are ok. any number of threads
            // can race to do this - thats fine, they'll all get the same result.
            bool ok = true;
            if (shardingState.needShardChunkManager(_ns)) {
                ShardChunkManagerPtr chunkManager = shardingState.getShardChunkManager(_ns);
                ok = chunkManager == NULL || chunkManager->hasShardKey(_pk);
            }
            _fastupdatesOkState.swap(ok ? 1 : 0);
            return ok;
        } else {
            // result already computed, fastupdates are ok if state is > 0
            dassert(state >= 0);
            return state > 0;
        }
    }

    BSONObj CollectionBase::getSimplePKFromQuery(const BSONObj &query, const BSONObj& pk) const {
        const int numPKFields = pk.nFields();
        scoped_array<BSONElement> pkElements(new BSONElement[numPKFields]);
        int numPKElementsFound = 0;
        for (BSONObjIterator queryIterator(query); queryIterator.more(); ) {
            const BSONElement &q = queryIterator.next();
            if (!q.isSimpleType() ||
                (q.type() == Object && q.Obj().firstElementFieldName()[0] == '$')) {
                continue; // not a 'simple' query element
            }
            BSONObjIterator pkIterator(pk);
            for (int i = 0; i < numPKFields; i++) {
                const BSONElement &p = pkIterator.next();
                if (pkElements[i].ok()) {
                    continue; // already set
                } else if (str::equals(q.fieldName(), p.fieldName())) {
                    pkElements[i] = q;
                    numPKElementsFound++;
                }
            }
        }
        if (numPKElementsFound == numPKFields) {
            // We found a simple element in the query for each part of the pk.
            BSONObjBuilder b;
            for (int i = 0; i < numPKFields; i++) {
                b.appendAs(pkElements[i], "");
            }
            return b.obj();
        }
        return BSONObj();
    }

    BSONObj CollectionBase::getValidatedPKFromObject(const BSONObj &obj) const {
        BSONObjSet keys;
        getPKIndexBase().getKeysFromObject(obj, keys);
        const BSONObj pk = keys.begin()->getOwned();
        uassert(17205, str::stream() << "primary key " << pk << " cannot be multi-key",
                       keys.size() == 1); // this enforces no arrays in the primary key
        for (BSONObjIterator i(pk); i.more(); ) {
            const BSONElement e = i.next();
            uassert(17208, "can't use a regex for any portion of the primary key",
                           e.type() != RegEx);
            uassert(17210, "can't use undefined for any portion of the primary key",
                           e.type() != Undefined);
        }
        return pk;
    }

    int CollectionBase::findByPKCallback(const DBT *key, const DBT *value, void *extra) {
        struct findByPKCallbackExtra *info = reinterpret_cast<findByPKCallbackExtra *>(extra);
        try {
            if (key != NULL) {
                struct findByPKCallbackExtra *info = reinterpret_cast<findByPKCallbackExtra *>(extra);
                info->obj = BSONObj(reinterpret_cast<char *>(value->data)).getOwned();
            }
            return 0;
        } catch (std::exception &e) {
            info->ex = &e;
        }
        return -1;
    }

    bool CollectionBase::findByPK(const BSONObj &key, BSONObj &result) const {
        TOKULOG(3) << "CollectionBase::findByPK looking for " << key << endl;

        storage::Key sKey(key, NULL);
        DBT key_dbt = sKey.dbt();
        DB *db = getPKIndexBase().db();

        BSONObj obj;
        struct findByPKCallbackExtra extra(obj);
        const int flags = cc().opSettings().getQueryCursorMode() != DEFAULT_LOCK_CURSOR ?
                          DB_SERIALIZABLE | DB_RMW : 0;
        const int r = db->getf_set(db, cc().txn().db_txn(), flags, &key_dbt,
                                   findByPKCallback, &extra);
        if (extra.ex != NULL) {
            throw *extra.ex;
        }
        if (r != 0 && r != DB_NOTFOUND) {
            storage::handle_ydb_error(r);
        }

        if (!obj.isEmpty()) {
            result = obj;
            return true;
        }
        return false;
    }

    void CollectionBase::insertIntoIndexes(const BSONObj &pk, const BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        *indexBitChanged = false; // just for initialization
        dassert(!pk.isEmpty());
        dassert(!obj.isEmpty());

        if (isSystemUsersCollection(_ns)) {
            uassertStatusOK(AuthorizationManager::checkValidPrivilegeDocument(nsToDatabaseSubstring(_ns), obj));
        }

        const int n = nIndexesBeingBuilt();
        DB *dbs[n];
        storage::DBTArrays keyArrays(n);
        storage::DBTArrays valArrays(n);
        uint32_t put_flags[n];

        storage::Key sPK(pk, NULL);
        DBT src_key = storage::dbt_make(sPK.buf(), sPK.size());
        DBT src_val = storage::dbt_make(obj.objdata(), obj.objsize());

        for (int i = 0; i < n; i++) {
            const bool isPK = i == 0;
            const bool prelocked = flags & Collection::NO_LOCKTREE;
            const bool doUniqueChecks = !(flags & Collection::NO_UNIQUE_CHECKS) &&
                                        !(isPK && (flags & Collection::NO_PK_UNIQUE_CHECKS));

            IndexDetailsBase &idx = *_indexes[i];
            dbs[i] = idx.db();

            // Primary key uniqueness check will be done at the ydb layer.
            // Secondary key uniqueness checks are done below, if necessary.
            put_flags[i] = (isPK && doUniqueChecks ? DB_NOOVERWRITE : 0) |
                           (prelocked ? DB_PRELOCKED_WRITE : 0);

            // It is not our responsibility to set the multikey bits
            // for a hot index. Further, a hot index cannot be unique,
            if (i >= _nIndexes) {
                continue;
            }

            if (!isPK) {
                BSONObjSet idxKeys;
                idx.getKeysFromObject(obj, idxKeys);
                if (idx.unique() && doUniqueChecks) {
                    for (BSONObjSet::const_iterator o = idxKeys.begin(); o != idxKeys.end(); ++o) {
                        idx.uniqueCheck(*o, pk);
                    }
                }
                if (idxKeys.size() > 1) {
                    setIndexIsMultikey(i, indexBitChanged);
                }
                // Store the keys we just generated, so we won't do it twice in
                // the generate keys callback. See storage::generate_keys()
                DBT_ARRAY *array = &keyArrays[i];
                storage::dbt_array_clear_and_resize(array, idxKeys.size());
                for (BSONObjSet::const_iterator it = idxKeys.begin(); it != idxKeys.end(); it++) {
                    const storage::Key sKey(*it, &pk);
                    storage::dbt_array_push(array, sKey.buf(), sKey.size());
                }
            }
        }

        DB_ENV *env = storage::env;
        const int r = env->put_multiple(env, dbs[0], cc().txn().db_txn(),
                                        &src_key, &src_val,
                                        n, dbs, keyArrays.arrays(), valArrays.arrays(), put_flags);
        if (r == EINVAL) {
            uasserted( 16900, str::stream() << "Indexed insertion failed." <<
                              " This may be due to keys > 32kb. Check the error log." );
        } else if (r != 0) {
            storage::handle_ydb_error(r);
        }

        // Index usage accounting. If a key was generated for this 
        // operation, then the index was used, otherwise it wasn't.
        // The PK is always used, only secondarys may have keys generated.
        getPKIndex().noteInsert();
        for (int i = 0; i < n; i++) {
            const DBT_ARRAY *array = &keyArrays[i];
            if (array->size > 0) {
                IndexDetailsBase &idx = *_indexes[i];
                dassert(!isPKIndex(idx));
                idx.noteInsert();
            }
        }
    }

    void CollectionBase::deleteFromIndexes(const BSONObj &pk, const BSONObj &obj, uint64_t flags) {
        dassert(!pk.isEmpty());
        dassert(!obj.isEmpty());

        const int n = nIndexesBeingBuilt();
        DB *dbs[n];
        storage::DBTArrays keyArrays(n);
        uint32_t del_flags[n];

        storage::Key sPK(pk, NULL);
        DBT src_key = storage::dbt_make(sPK.buf(), sPK.size());
        DBT src_val = storage::dbt_make(obj.objdata(), obj.objsize());

        for (int i = 0; i < n; i++) {
            const bool isPK = i == 0;
            const bool prelocked = flags & Collection::NO_LOCKTREE;
            IndexDetailsBase &idx = *_indexes[i];
            dbs[i] = idx.db();
            del_flags[i] = DB_DELETE_ANY | (prelocked ? DB_PRELOCKED_WRITE : 0);
            DEV {
                // for debug builds, remove the DB_DELETE_ANY flag
                // so that debug builds do a query to make sure the
                // row is there. It is a nice check to ensure correctness
                // on debug builds.
                del_flags[i] &= ~DB_DELETE_ANY;
            }
            if (!isPK) {
                BSONObjSet idxKeys;
                idx.getKeysFromObject(obj, idxKeys);

                if (idxKeys.size() > 1) {
                    verify(isMultiKey(i));
                }

                // Store the keys we just generated, so we won't do it twice in
                // the generate keys callback. See storage::generate_keys()
                DBT_ARRAY *array = &keyArrays[i];
                storage::dbt_array_clear_and_resize(array, idxKeys.size());
                for (BSONObjSet::const_iterator it = idxKeys.begin(); it != idxKeys.end(); it++) {
                    const storage::Key sKey(*it, &pk);
                    storage::dbt_array_push(array, sKey.buf(), sKey.size());
                }
            }
        }

        DB_ENV *env = storage::env;
        const int r = env->del_multiple(env, dbs[0], cc().txn().db_txn(),
                                        &src_key, &src_val,
                                        n, dbs, keyArrays.arrays(), del_flags);
        if (r != 0) {
            storage::handle_ydb_error(r);
        }

        // Index usage accounting. If a key was generated for this 
        // operation, then the index was used, otherwise it wasn't.
        // The PK is always used, only secondarys may have keys generated.
        getPKIndex().noteDelete();
        for (int i = 0; i < n; i++) {
            const DBT_ARRAY *array = &keyArrays[i];
            if (array->size > 0) {
                IndexDetailsBase &idx = *_indexes[i];
                dassert(!isPKIndex(idx));
                idx.noteDelete();
            }
        }
    }

    // uasserts on duplicate key
    static bool orderedSetContains(const BSONObjSet &set, const BSONObj &obj) {
        bool contains = false;
        for (BSONObjSet::iterator i = set.begin(); i != set.end(); i++) {
            const int c = i->woCompare(obj);
            if (c >= 0) {
                contains = c == 0;
                break;
            }
        }
        return contains;
    }

    // deletes an object from this collection, taking care of secondary indexes if they exist
    void CollectionBase::deleteObject(const BSONObj &pk, const BSONObj &obj, uint64_t flags) {
        deleteFromIndexes(pk, obj, flags);
    }

    void CollectionBase::updateObject(const BSONObj &pk, const BSONObj &oldObj, const BSONObj &newObj,
                                      const bool logop, const bool fromMigrate, uint64_t flags, bool* indexBitChanged) {
        TOKULOG(4) << "CollectionBase::updateObject pk "
            << pk << ", old " << oldObj << ", new " << newObj << endl;
        *indexBitChanged = false;

        dassert(!pk.isEmpty());
        dassert(!oldObj.isEmpty());
        dassert(!newObj.isEmpty());

        if (isSystemUsersCollection(_ns)) {
            uassertStatusOK(AuthorizationManager::checkValidPrivilegeDocument(nsToDatabaseSubstring(_ns), newObj));
        }

        const int n = nIndexesBeingBuilt();
        DB *dbs[n];
        storage::DBTArrays keyArrays(n * 2);
        storage::DBTArrays valArrays(n);
        uint32_t update_flags[n];

        storage::Key sPK(pk, NULL);
        DBT src_key = storage::dbt_make(sPK.buf(), sPK.size());
        DBT new_src_val = storage::dbt_make(newObj.objdata(), newObj.objsize());
        DBT old_src_val = storage::dbt_make(oldObj.objdata(), oldObj.objsize());

        // Generate keys for each index, prepare data structures for del multiple.
        // We will end up abandoning del multiple if there are any multikey indexes.
        for (int i = 0; i < n; i++) {
            const bool isPK = i == 0;
            const bool prelocked = flags & Collection::NO_LOCKTREE;
            const bool doUniqueChecks = !(flags & Collection::NO_UNIQUE_CHECKS) &&
                                        !(isPK && (flags & Collection::NO_PK_UNIQUE_CHECKS));

            IndexDetailsBase &idx = *_indexes[i];
            dbs[i] = idx.db();
            update_flags[i] = prelocked ? DB_PRELOCKED_WRITE : 0;

            // It is not our responsibility to set the multikey bits
            // for a hot index. Further, a hot index cannot be unique,
            if (i >= _nIndexes) {
                continue;
            }

            // We only need to generate keys etc for secondary indexes when:
            // - The keys may have changed, which is possible if the keys unaffected
            //   hint was not given.
            // - The index is clustering. It doesn't matter if keys have changed because
            //   we need to update the clustering document.
            const bool keysMayHaveChanged = !(flags & Collection::KEYS_UNAFFECTED_HINT);
            if (!isPK && (keysMayHaveChanged || idx.clustering())) {
                BSONObjSet oldIdxKeys;
                BSONObjSet newIdxKeys;
                idx.getKeysFromObject(oldObj, oldIdxKeys);
                idx.getKeysFromObject(newObj, newIdxKeys);
                if (idx.unique() && doUniqueChecks && keysMayHaveChanged) {
                    // Only perform the unique check for those keys that actually changed.
                    for (BSONObjSet::iterator o = newIdxKeys.begin(); o != newIdxKeys.end(); ++o) {
                        const BSONObj &k = *o;
                        if (!orderedSetContains(oldIdxKeys, k)) {
                            idx.uniqueCheck(k, pk);
                        }
                    }
                }
                if (newIdxKeys.size() > 1) {
                    setIndexIsMultikey(i, indexBitChanged);
                }

                // Store the keys we just generated, so we won't do it twice in
                // the generate keys callback. See storage::generate_keys()
                DBT_ARRAY *array = &keyArrays[i];
                storage::dbt_array_clear_and_resize(array, newIdxKeys.size());
                for (BSONObjSet::const_iterator it = newIdxKeys.begin(); it != newIdxKeys.end(); it++) {
                    const storage::Key sKey(*it, &pk);
                    storage::dbt_array_push(array, sKey.buf(), sKey.size());
                }
                array = &keyArrays[i + n];
                storage::dbt_array_clear_and_resize(array, oldIdxKeys.size());
                for (BSONObjSet::const_iterator it = oldIdxKeys.begin(); it != oldIdxKeys.end(); it++) {
                    const storage::Key sKey(*it, &pk);
                    storage::dbt_array_push(array, sKey.buf(), sKey.size());
                }
            }
        }

        // The pk doesn't change, so old_src_key == new_src_key.
        DB_ENV *env = storage::env;
        const int r = env->update_multiple(env, dbs[0], cc().txn().db_txn(),
                                           &src_key, &old_src_val,
                                           &src_key, &new_src_val,
                                           n, dbs, update_flags,
                                           n * 2, keyArrays.arrays(), n, valArrays.arrays());
        if (r == EINVAL) {
            uasserted( 16908, str::stream() << "Indexed insertion (on update) failed." <<
                              " This may be due to keys > 32kb. Check the error log." );
        } else if (r != 0) {
            storage::handle_ydb_error(r);
        }

        if (logop) {
            OpLogHelpers::logUpdate(_ns.c_str(), pk, oldObj, newObj, fromMigrate);
        }
    }

    void CollectionBase::updateObjectMods(const BSONObj &pk, const BSONObj &updateObj,
                                          const bool logop, const bool fromMigrate,
                                          uint64_t flags) {
        IndexDetailsBase &pkIdx = getPKIndexBase();
        pkIdx.updatePair(pk, NULL, updateObj, flags);

        if (logop) {
            OpLogHelpers::logUpdateMods(_ns.c_str(), pk, updateObj, fromMigrate);
        }
    }

    // only set indexBitsChanged if true, NEVER set to false
    void CollectionBase::setIndexIsMultikey(const int idxNum, bool* indexBitChanged) {
        // Under no circumstasnces should the primary key become multikey.
        verify(idxNum > 0);
        dassert(idxNum < Collection::NIndexesMax);
        const unsigned long long x = ((unsigned long long) 1) << idxNum;
        if (_multiKeyIndexBits & x) {
            *indexBitChanged = false;
            return;
        }
        if (!Lock::isWriteLocked(_ns)) {
            throw RetryWithWriteLock();
        }
        *indexBitChanged = true;
        _multiKeyIndexBits |= x;
    }

    void CollectionBase::checkIndexUniqueness(const IndexDetailsBase &idx) {
        shared_ptr<Cursor> c(Cursor::make(this, idx, 1, false));
        BSONObj prevKey = c->currKey().getOwned();
        c->advance();
        for ( ; c->ok(); c->advance() ) {
            BSONObj currKey = c->currKey(); 
            if (currKey == prevKey) {
                idx.uassertedDupKey(currKey);
            }
            prevKey = currKey.getOwned();
        }
    }

    // Wrapper for offline (write locked) indexing.
    void CollectionBase::createIndex(const BSONObj &info) {
        const string sourceNS = info["ns"].String();

        if (!Lock::isWriteLocked(_ns)) {
            throw RetryWithWriteLock();
        }

        shared_ptr<CollectionIndexer> indexer = newIndexer(info, false);
        indexer->prepare();
        indexer->build();
        indexer->commit();
    }

    void CollectionBase::dropIndexDetails(int idxNum) {
        IndexDetailsBase& idx = *_indexes[idxNum];
        idx.kill_idx();
        _indexes.erase(_indexes.begin() + idxNum);
        _nIndexes--;

        // Removes the nth bit, and shifts any bits higher than it down a slot.
        _multiKeyIndexBits = ((_multiKeyIndexBits & ((1ULL << idxNum) - 1)) |
                             ((_multiKeyIndexBits >> (idxNum + 1)) << idxNum));
    }

    void CollectionBase::acquireTableLock() {
        // Acquire full table locks on each index so that only this
        // transcation can write to them until the load/txn commits.
        for (int i = 0; i < nIndexes(); i++) {
            IndexDetailsBase &idx = *_indexes[i];
            idx.acquireTableLock();
        }
    }

    shared_ptr<Cursor> CollectionBase::makeCursor(const int direction, const bool countCursor) {
        if (countCursor) {
            return shared_ptr<Cursor>(new IndexScanCountCursor(this, findSmallestOneToOneIndex()));
        } else {
            return shared_ptr<Cursor>(new BasicCursor(this, direction));
        }
    }
    
    // index-scan
    shared_ptr<Cursor> CollectionBase::makeCursor(const IndexDetails &idx,
                                    const int direction, 
                                    const bool countCursor)
    {
        if (countCursor) {
            return shared_ptr<Cursor>(new IndexScanCountCursor(this, idx));
        } else {
            return shared_ptr<Cursor>(new IndexScanCursor(this, idx, direction));
        }
    }
    
    // index range scan between start/end
    shared_ptr<Cursor> CollectionBase::makeCursor(const IndexDetails &idx,
                                   const BSONObj &startKey, const BSONObj &endKey,
                                   const bool endKeyInclusive,
                                   const int direction, const int numWanted,
                                   const bool countCursor)
    {
        if (countCursor) {
            return shared_ptr<Cursor>(new IndexCountCursor(this, idx, startKey, endKey,
                                                           endKeyInclusive));
        } else {
            return shared_ptr<Cursor>(new IndexCursor(this, idx, startKey, endKey,
                                                      endKeyInclusive, direction, numWanted));
        }
    }
    
    // index range scan by field bounds
    shared_ptr<Cursor> CollectionBase::makeCursor(const IndexDetails &idx,
                                   const shared_ptr<FieldRangeVector> &bounds,
                                   const int singleIntervalLimit,
                                   const int direction, const int numWanted,
                                   const bool countCursor)
    {
        if (countCursor) {
            return shared_ptr<Cursor>(new IndexCountCursor(this, idx, bounds));
        } else {
            return shared_ptr<Cursor>(new IndexCursor(this, idx, bounds,
                                                      singleIntervalLimit, direction, numWanted));
        }
    }

    void Collection::dropIndex(const int idxNum) {
        // make sure a hot index is not in progress
        verify(nIndexes() == nIndexesBeingBuilt());
        verify(idxNum < nIndexes());

        IndexDetails &idx = _cd->idx(idxNum);

        // Note this ns in the rollback so if this transaction aborts, we'll
        // close this ns, forcing the next user to reload in-memory metadata.
        CollectionMapRollback &rollback = cc().txn().collectionMapRollback();
        rollback.noteNs(_ns);

        // Remove this index from the system catalogs
        removeFromNamespacesCatalog(idx.indexNamespace());
        if (nsToCollectionSubstring(_ns) != "system.indexes") {
            removeFromIndexesCatalog(_ns, idx.indexName());
        }

        _cd->dropIndexDetails(idxNum);
        
        resetTransient();
        // Updated whatever in memory structures are necessary, now update the collectionMap.
        collectionMap(_ns)->update_ns(_ns, serialize(), true);
    }

    // Normally, we cannot drop the _id_ index.
    // The parameters mayDeleteIdIndex is here for the case where we call dropIndexes
    // through dropCollection, in which case we are dropping an entire collection,
    // hence the _id_ index will have to go.
    bool Collection::dropIndexes(const StringData& name, string &errmsg, BSONObjBuilder &result, bool mayDeleteIdIndex) {
        Lock::assertWriteLocked(_ns);
        TOKULOG(1) << "dropIndexes " << name << endl;

        uassert( 16904, "Cannot drop indexes: a hot index build in progress.",
                        nIndexesBeingBuilt() == nIndexes());

        ClientCursor::invalidate(_ns);
        const int idxNum = _cd->findIndexByName(name);
        if (name == "*") {
            result.append("nIndexesWas", (double) nIndexes());
            for (int i = 0; i < nIndexes(); ) {
                IndexDetails &idx = _cd->idx(i);
                if (mayDeleteIdIndex || (!idx.isIdIndex() && !isPKIndex(idx))) {
                    dropIndex(i);
                } else {
                    i++;
                }
            }
            // Assuming id/pk index isn't multikey
            verify(_cd->getMultiKeyIndexBits() == 0);
            result.append("msg", (mayDeleteIdIndex
                                  ? "indexes dropped for collection"
                                  : "non-_id indexes dropped for collection"));
        } else {
            if (idxNum >= 0) {
                result.append("nIndexesWas", (double) nIndexes());
                IndexDetails &idx = _cd->idx(idxNum);
                if ( !mayDeleteIdIndex && (idx.isIdIndex() || isPKIndex(idx)) ) {
                    errmsg = "may not delete _id or $_ index";
                    return false;
                }
                dropIndex(idxNum);
            } else {
                log() << "dropIndexes: " << name << " not found" << endl;
                errmsg = "index not found";
                return false;
            }
        }

        return true;
    }

    void Collection::drop(string &errmsg, BSONObjBuilder &result, const bool mayDropSystem) {
        // Check that we are allowed to drop the namespace.
        StringData database = nsToDatabaseSubstring(_ns);
        verify(database == cc().database()->name());
        if (NamespaceString::isSystem(_ns) && !mayDropSystem) {
            if (nsToCollectionSubstring(_ns) == "system.profile") {
                uassert(10087, "turn off profiling before dropping system.profile collection", cc().database()->profile() == 0);
            } else {
                uasserted(12502, "can't drop system ns");
            }
        }

        // Invalidate cursors, then drop all of the indexes.
        ClientCursor::invalidate(_ns);
        dropIndexes("*", errmsg, result, true);
        verify(nIndexes() == 0);
        removeFromNamespacesCatalog(_ns);

        Top::global.collectionDropped(_ns);
        result.append("ns", _ns);

        // Kill the ns from the collectionMap.
        //
        // Will delete "this" Collection object, since it's lifetime is managed
        // by a shared pointer in the map we're going to delete from.
        collectionMap(_ns)->kill_ns(_ns);
    }

    // rebuild the given index, online.
    // - if there are options, change those options in the index and update the system catalog.
    // - otherwise, send an optimize message and run hot optimize.
    bool CollectionBase::rebuildIndex(int i, const BSONObj &options, BSONObjBuilder &wasBuilder) {
        IndexDetailsBase& idx = *_indexes[i];
        if (options.isEmpty()) {
            LOG(1) << _ns << ": optimizing index " << idx.keyPattern() << endl;
            const bool ascending = !Ordering::make(idx.keyPattern()).descending(0);
            const bool isPK = isPKIndex(idx);

            storage::Key leftSKey(ascending ? minKey : maxKey,
                                  isPK ? NULL : &minKey);
            storage::Key rightSKey(ascending ? maxKey : minKey,
                                   isPK ? NULL : &maxKey);
            uint64_t loops_run;
            idx.optimize(leftSKey, rightSKey, true, 0, &loops_run);
            return false;
        } else {
            LOG(1) << _ns << ": altering index " << idx.keyPattern() << ", options " << options << endl;
            return idx.changeAttributes(options, wasBuilder);
        }
    }

    void Collection::rebuildIndexes(const StringData &name, const BSONObj &options, BSONObjBuilder &result) {
        bool pkIndexChanged = false;
        if (name == "*") {
            BSONArrayBuilder ab;
            // "*" means everything
            for (int i = 0; i < nIndexes(); i++) {
                IndexDetails &idx = _cd->idx(i);
                BSONObjBuilder wasBuilder(ab.subobjStart());
                wasBuilder.append("name", idx.indexName());
                if (_cd->rebuildIndex(i, options, wasBuilder)) {
                    if (isPKIndex(idx)) {
                        pkIndexChanged = true;
                    }
                    removeFromIndexesCatalog(_ns, idx.indexName());
                    addToIndexesCatalog(idx.info());
                }
                wasBuilder.doneFast();
            }
            if (!options.isEmpty()) {
                result.appendArray("was", ab.done());
            }
        } else {
            // optimize a single index.
            // our caller should ensure that the index exists.
            const int i = _cd->findIndexByName(name);
            uassert(17231, str::stream() << "index not found: " << name,
                           i >= 0);
            uassert(17232, str::stream() << "cannot rebuild a background index: " << name,
                           i < nIndexes()); // i == _nIndexes is the hot index
            IndexDetails &idx = _cd->idx(i);
            BSONObjBuilder wasBuilder;
            if (_cd->rebuildIndex(i, options, wasBuilder)) {
                if (isPKIndex(idx)) {
                    pkIndexChanged = true;
                }
                removeFromIndexesCatalog(_ns, idx.indexName());
                addToIndexesCatalog(idx.info());
            }
            if (!options.isEmpty()) {
                result.append("was", wasBuilder.done());
            }
        }
        if (pkIndexChanged) {
            BSONObjBuilder optionsBuilder;
            if (_options.isEmpty()) {
                optionsBuilder.append("create", nsToCollectionSubstring(_ns));
                for (BSONObjIterator it(options); it.more(); ++it) {
                    optionsBuilder.append(*it);
                }
            } else {
                optionsBuilder.append(_options["create"]);
                for (BSONObjIterator it(_options); it.more(); ++it) {
                    BSONElement e = *it;
                    StringData fn(e.fieldName());
                    if (options.hasField(fn)) {
                        optionsBuilder.append(options[fn]);
                    } else {
                        optionsBuilder.append(_options[fn]);
                    }
                }
                for (BSONObjIterator it(options); it.more(); ++it) {
                    BSONElement e = *it;
                    StringData fn(e.fieldName());
                    if (!_options.hasField(fn)) {
                        optionsBuilder.append(e);
                    }
                }
            }
            _options = optionsBuilder.obj();
            removeFromNamespacesCatalog(_ns);
            addToNamespacesCatalog(_ns, &_options);
        }
    }

    void Collection::fillCollectionStats(
        Stats &aggStats,
        BSONObjBuilder *result,
        int scale) const
    {
        Stats stats;
        stats.nIndexes += nIndexes();
        // also sum up some stats of secondary indexes,
        // calculate their total data size and storage size
        BSONArrayBuilder ab;
        for (int i = 0; i < nIndexes(); i++) {
            IndexDetails &currIdx = idx(i);
            IndexDetails::Stats idxStats = currIdx.getStats();
            BSONObjBuilder infoBuilder(ab.subobjStart());
            idxStats.appendInfo(infoBuilder, scale);
            infoBuilder.done();
            if (isPKIndex(currIdx)) {
                stats.count += idxStats.count;
                stats.size += idxStats.dataSize;
                stats.storageSize += idxStats.storageSize;
            } else {
                // Only count secondary indexes here
                stats.indexSize += idxStats.dataSize;
                stats.indexStorageSize += idxStats.storageSize;
            }
        }

        if (result != NULL) {
            // unfortunately, this protocol's format is a little unorthodox
            // TODO: unify this with appendInfo, if we can somehow unify the interface
            result->appendNumber("count", (long long) stats.count);
            result->appendNumber("nindexes", nIndexes());
            result->appendNumber("nindexesbeingbuilt", nIndexesBeingBuilt());
            result->appendNumber("size", (long long) stats.size/scale);
            result->appendNumber("storageSize", (long long) stats.storageSize/scale);
            result->appendNumber("totalIndexSize", (long long) stats.indexSize/scale);
            result->appendNumber("totalIndexStorageSize", (long long) stats.indexStorageSize/scale);
            result->appendArray("indexDetails", ab.done());

            fillSpecificStats(*result, scale);
        }

        // This must happen last in order to scale the values in the result bson above
        aggStats += stats;
    }

    void Collection::noteIndexBuilt() {
        collectionMap(_ns)->update_ns(_ns, serialize(true), true);
        resetTransient();
    }

    void Collection::Stats::appendInfo(BSONObjBuilder &b, int scale) const {
        b.appendNumber("objects", (long long) count);
        b.appendNumber("avgObjSize", count == 0 ? 0.0 : double(size) / double(count));
        b.appendNumber("dataSize", (long long) size / scale);
        b.appendNumber("storageSize", (long long) storageSize / scale);
        b.appendNumber("indexes", (long long) nIndexes);
        b.appendNumber("indexSize", (long long) indexSize / scale);
        b.appendNumber("indexStorageSize", (long long) indexStorageSize / scale);
    }

    void Collection::addDefaultIndexesToCatalog() {
        // Either a single primary key or a hidden primary key + _id index.
        // TODO: this is now incorrect in the case of system.users collections, need to fix it and
        //uncomment it:
        //dassert(_nIndexes == 1 || (_nIndexes == 2 && findIdIndex() == 1));
        for (int i = 0; i < nIndexes(); i++) {
            addToIndexesCatalog(idx(i).info());
        }
    }

    bool CollectionBase::ensureIndex(const BSONObj &info) {
        const BSONObj keyPattern = info["key"].Obj();
        const int i = findIndexByKeyPattern(keyPattern);
        if (i >= 0) {
            return false;
        }
        createIndex(info);
        return true;
    }

    // Get an indexer over this collection. Implemented in indexer.cpp
    shared_ptr<CollectionIndexer> CollectionBase::newIndexer(const BSONObj &info,
                                                               const bool background) {
        if (background) {
            return shared_ptr<CollectionIndexer>(new HotIndexer(this, info));
        } else {
            return shared_ptr<CollectionIndexer>(new ColdIndexer(this, info));
        }
    }

    /* ------------------------------------------------------------------------- */

    bool userCreateNS(const StringData& ns, BSONObj options, string& err, bool logForReplication) {
        StringData coll = ns.substr(ns.find('.') + 1);
        massert( 16451 ,  str::stream() << "invalid ns: " << ns , NamespaceString::validCollectionName(ns));
        StringData cl = nsToDatabaseSubstring( ns );
        if (getCollection(ns) != NULL) {
            // Namespace already exists
            err = "collection already exists";
            return false;
        }

        if ( cmdLine.configsvr &&
             !( ns.startsWith( "config." ) ||
                ns.startsWith( "local." ) ||
                ns.startsWith( "admin." ) ) ) {
            uasserted(14037, "can't create user databases on a --configsvr instance");
        }

        {
            BSONElement e = options.getField("size");
            if (e.isNumber()) {
                long long size = e.numberLong();
                uassert(10083, "create collection invalid size spec", size >= 0);
            }
        }

        // This creates the namespace as well as its _id index
        _getOrCreateCollection(ns, options);
        if ( logForReplication ) {
            if ( options.getField( "create" ).eoo() ) {
                BSONObjBuilder b;
                b << "create" << coll;
                b.appendElements( options );
                options = b.obj();
            }
            string logNs = cl.toString() + ".$cmd";
            OpLogHelpers::logCommand(logNs.c_str(), options);
        }
        return true;
    }

    /* add a new namespace to the system catalog (<dbname>.system.namespaces).
       options: { capped : ..., size : ... }
    */
    void addToNamespacesCatalog(const StringData& ns, const BSONObj *options) {
        LOG(1) << "New namespace: " << ns << endl;
        StringData coll = nsToCollectionSubstring(ns);
        if (coll.startsWith("system.namespaces")) {
            // system.namespaces holds all the others, so it is not explicitly listed in the catalog.
            return;
        }

        BSONObjBuilder b;
        b.append("name", ns);
        if ( options ) {
            b.append("options", *options);
        }
        BSONObj info = b.done();

        string system_ns = getSisterNS(ns, "system.namespaces");
        Collection *cl = _getOrCreateCollection(system_ns);
        insertOneObject(cl, info);
    }

    void addToIndexesCatalog(const BSONObj &info) {
        const StringData &indexns = info["ns"].Stringdata();
        if (nsToCollectionSubstring(indexns).startsWith("system.indexes")) {
            // system.indexes holds all the others, so it is not explicitly listed in the catalog.
            return;
        }

        string ns = getSisterNS(indexns, "system.indexes");
        Collection *cl = _getOrCreateCollection(ns);
        BSONObj objMod = info;
        insertOneObject(cl, objMod);
    }

    static void removeFromNamespacesCatalog(const StringData &ns) {
        StringData coll = nsToCollectionSubstring(ns);
        if (!coll.startsWith("system.namespaces")) {
            string system_namespaces = getSisterNS(cc().database()->name(), "system.namespaces");
            _deleteObjects(system_namespaces.c_str(),
                           BSON("name" << ns), false, false);
        }
    }

    static void removeFromIndexesCatalog(const StringData &ns, const StringData &name) {
        string system_indexes = getSisterNS(cc().database()->name(), "system.indexes");
        BSONObj obj = BSON("ns" << ns << "name" << name);
        TOKULOG(2) << "removeFromIndexesCatalog removing " << obj << endl;
        const int n = _deleteObjects(system_indexes.c_str(), obj, false, false);
        verify(n == 1);
    }

    static BSONObj replaceNSField(const BSONObj &obj, const StringData &to) {
        BSONObjBuilder b;
        BSONObjIterator i( obj );
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( strcmp( e.fieldName(), "ns" ) != 0 ) {
                b.append( e );
            } else {
                b << "ns" << to;
            }
        }
        return b.obj();
    }

    void renameCollection(const StringData& from, const StringData& to) {
        Lock::assertWriteLocked(from);

        Collection *from_cl = getCollection(from);
        verify( from_cl != NULL );
        verify( getCollection(to) == NULL );

        uassert( 16896, "Cannot rename a collection under-going bulk load.",
                        from != cc().bulkLoadNS() );
        uassert( 16918, "Cannot rename a collection with a background index build in progress",
                        !from_cl->indexBuildInProgress() );

        // Kill open cursors before we close and rename the namespace
        ClientCursor::invalidate( from );

        string sysIndexes = getSisterNS(from, "system.indexes");
        string sysNamespaces = getSisterNS(from, "system.namespaces");

        // Generate the serialized form of the namespace, and then close it.
        // This will close the underlying dictionaries and allow us to
        // rename them in the environment.
        BSONObj serialized = from_cl->serialize();
        bool closed = collectionMap(from)->close_ns(from);
        verify(closed);

        // Rename each index in system.indexes and system.namespaces
        {
            BSONObj nsQuery = BSON( "ns" << from );
            vector<BSONObj> indexSpecs;
            {
                // Find all entries in sysIndexes for the from ns
                Client::Context ctx( sysIndexes );
                for (shared_ptr<Cursor> c( getOptimizedCursor( sysIndexes, nsQuery ) );
                     c->ok(); c->advance()) {
                    if (c->currentMatches()) {
                        indexSpecs.push_back( c->current().copy() );
                    }
                }
            }
            for ( vector<BSONObj>::const_iterator it = indexSpecs.begin() ; it != indexSpecs.end(); it++) {
                BSONObj oldIndexSpec = *it;
                string idxName = oldIndexSpec["name"].String();
                string oldIdxNS = IndexDetails::indexNamespace(from, idxName);
                string newIdxNS = IndexDetails::indexNamespace(to, idxName);

                TOKULOG(1) << "renaming " << oldIdxNS << " to " << newIdxNS << endl;
                storage::db_rename(oldIdxNS, newIdxNS);

                BSONObj newIndexSpec = replaceNSField( oldIndexSpec, to );
                removeFromIndexesCatalog(from, idxName);
                removeFromNamespacesCatalog(oldIdxNS);
                addToIndexesCatalog(newIndexSpec);
                addToNamespacesCatalog(newIdxNS, newIndexSpec.isEmpty() ? 0 : &newIndexSpec);
            }
        }

        // Rename the namespace in system.namespaces
        BSONObj newSpec;
        {
            BSONObj oldSpec;
            verify(Collection::findOne(sysNamespaces, BSON("name" << from), oldSpec));
            BSONObjBuilder b;
            BSONObjIterator i( oldSpec.getObjectField( "options" ) );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( strcmp( e.fieldName(), "create" ) != 0 ) {
                    b.append( e );
                }
                else {
                    b << "create" << to;
                }
            }
            newSpec = b.obj();
            removeFromNamespacesCatalog(from);
            addToNamespacesCatalog(to, newSpec.isEmpty() ? 0 : &newSpec);
        }

        // Update the namespace index
        {
            BSONArrayBuilder newIndexesArray;
            vector<BSONElement> indexes = serialized["indexes"].Array();
            for (vector<BSONElement>::const_iterator it = indexes.begin(); it != indexes.end(); it++) {
                newIndexesArray.append(replaceNSField(it->Obj(), to));
            }
            BSONObj newSerialized = Collection::serialize(to, newSpec, serialized["pk"].Obj(),
                                                              serialized["multiKeyIndexBits"].Long(),
                                                              newIndexesArray.arr());
            // Kill the old entry and replace it with the new name and modified spec.
            // The next user of the newly-named namespace will need to open it.
            CollectionMap *cm = collectionMap( from );
            cm->kill_ns(from);
            cm->update_ns(to, newSerialized, false);
            verify(getCollection(to) != NULL);
            verify(getCollection(from) == NULL);
        }
    }

    void beginBulkLoad(const StringData &ns, const vector<BSONObj> &indexes,
                       const BSONObj &options) {
        uassert( 16873, "Cannot bulk load a collection that already exists.",
                        getCollection(ns) == NULL );
        uassert( 16998, "Cannot bulk load a system collection",
                        !NamespaceString::isSystem(ns) );
        uassert( 16999, "Cannot bulk load a capped collection",
                        !options["capped"].trueValue() );
        uassert( 17000, "Cannot bulk load a natural order collection",
                        !options["natural"].trueValue() );

        // Don't log the create. The begin/commit/abort load commands are already logged.
        string errmsg;
        const bool created = userCreateNS(ns, options, errmsg, false);
        verify(created);

        CollectionMap *cm = collectionMap(ns);
        Collection *cl = cm->getCollection(ns);
        for (vector<BSONObj>::const_iterator i = indexes.begin(); i != indexes.end(); i++) {
            BSONObj info = *i;
            const BSONElement &e = info["ns"];
            if (e.ok()) {
                uassert( 16886, "Each index spec's ns field, if provided, must match the loaded ns.",
                                e.type() == mongo::String && e.Stringdata() == ns );
            } else {
                // Add the ns field if it wasn't provided.
                BSONObjBuilder b;
                b.append("ns", ns);
                b.appendElements(info);
                info = b.obj();
            }
            uassert( 16887, "Each index spec must have a string name field.",
                            info["name"].ok() && info["name"].type() == mongo::String );
            if (cl->ensureIndex(info)) {
                addToIndexesCatalog(info);
            }
        }
        cl->acquireTableLock();

        // Now the ns exists. Close it and re-open it in "bulk load" mode.
        const bool closed = cm->close_ns(ns);
        verify(closed);
        const bool opened = cm->open_ns(ns, true);
        verify(opened);
    }

    void commitBulkLoad(const StringData &ns) {
        CollectionMap *cm = collectionMap(ns);
        const bool closed = cm->close_ns(ns);
        verify(closed);
    }

    void abortBulkLoad(const StringData &ns) {
        CollectionMap *cm = collectionMap(ns);
        // Close the ns with aborting = true, which will hint to the
        // BulkLoadedCollection that it should abort the load.
        const bool closed = cm->close_ns(ns, true);
        verify(closed);
    }

    bool legalClientSystemNS( const StringData& ns , bool write ) {
        if( ns == "local.system.replset" ) return true;

        StringData collstr = nsToCollectionSubstring(ns);
        if ( collstr == "system.users" ) {
            return true;
        }

        if ( collstr == "system.js" ) {
            if ( write )
                Scope::storedFuncMod();
            return true;
        }

        return false;
    }

    int CollectionBase::findIndexByKeyPattern(const BSONObj& keyPattern) const {
        for (IndexVector::const_iterator it = _indexes.begin(); it != _indexes.end(); ++it) {
            const IndexDetails *index = it->get();
            if (index->keyPattern() == keyPattern) {
                return it - _indexes.begin();
            }
        }
        return -1;
    }

    IndexDetails &CollectionBase::findSmallestOneToOneIndex() const {
        // Default to choosing the primary key index (always at _indexes[0]);
        int chosenIndex = 0;

        // Check the secondary indexes. Any non-clustering secondary index is
        // better than using the primary key, since there's no object stored
        // and the key length can be at most the size of the object.
        uint64_t smallestIndexSize = std::numeric_limits<uint64_t>::max();
        for (int i = 1; i < nIndexes(); i++) {
            const IndexDetails &index = idx(i);
            IndexDetails::Stats st = index.getStats();
            if (!index.sparse() && !isMultiKey(i) && st.dataSize < smallestIndexSize) {
                smallestIndexSize = st.dataSize;
                chosenIndex = i;
            }
        }

        return idx(chosenIndex);
    }

    const IndexDetails* Collection::findIndexByPrefix( const BSONObj &keyPattern ,
                                                       bool requireSingleKey ) const {
        const IndexDetails* bestMultiKeyIndex = NULL;
        for (int i = 0; i < nIndexesBeingBuilt(); i++) {
            const IndexDetails &index = idx(i);
            if (keyPattern.isPrefixOf(index.keyPattern())) {
                if (!isMultikey(i)) {
                    return &index;
                } else {
                    bestMultiKeyIndex = &index;
                }
            }
        }
        return requireSingleKey ? NULL : bestMultiKeyIndex;
    }

    // @return offset in indexes[]
    int CollectionBase::findIndexByName(const StringData& name) const {
        for (IndexVector::const_iterator it = _indexes.begin(); it != _indexes.end(); ++it) {
            const IndexDetails *index = it->get();
            if (index->indexName() == name) {
                return it - _indexes.begin();
            }
        }
        return -1;
    }

    // ------------------------------------------------------------------------

    static BSONObj addIdField(const BSONObj &obj) {
        if (obj.hasField("_id")) {
            return obj;
        } else {
            // _id first, everything else after
            BSONObjBuilder b;
            OID oid;
            oid.init();
            b.append("_id", oid);
            b.appendElements(obj);
            return b.obj();
        }
    }

    static BSONObj inheritIdField(const BSONObj &oldObj, const BSONObj &newObj) {
        const BSONElement &e = newObj["_id"];
        if (e.ok()) {
            uassert( 13596 ,
                     str::stream() << "cannot change _id of a document old:" << oldObj << " new:" << newObj,
                     e.valuesEqual(oldObj["_id"]) );
            return newObj;
        } else {
            BSONObjBuilder b;
            b.append(oldObj["_id"]);
            b.appendElements(newObj);
            return b.obj();
        }
    }

    // ------------------------------------------------------------------------

    BSONObj IndexedCollection::determinePrimaryKey(const BSONObj &options) {
        const BSONObj idPattern = BSON("_id" << 1);
        BSONObj pkPattern = idPattern;
        if (options["primaryKey"].ok()) {
            uassert(17209, "defined primary key must be an object", options["primaryKey"].type() == Object);
            pkPattern = options["primaryKey"].Obj();
            bool pkPatternLast = false;
            for (BSONObjIterator i(pkPattern); i.more(); ) {
                const BSONElement e = i.next();
                if (!i.more()) {
                    // This is the last element. Needs to be _id: 1
                    pkPatternLast = e.wrap() == idPattern;
                }
            }
            uassert(17203, "defined primary key must end in _id: 1", pkPatternLast);
            uassert(17204, "defined primary key cannot be sparse", !options["sparse"].trueValue());
        }
        return pkPattern;
    }

    IndexedCollection::IndexedCollection(const StringData &ns, const BSONObj &options) :
        CollectionBase(ns, determinePrimaryKey(options), options),
        // determinePrimaryKey() was called, so whatever the pk is, it
        // exists in _indexes. Thus, we know we have an _id primary key
        // if we can find an index with pattern "_id: 1" at this point.
        _idPrimaryKey(findIndexByKeyPattern(BSON("_id" << 1)) >= 0) {
        const int idxNo = findIndexByKeyPattern(BSON("_id" << 1));
        if (idxNo < 0) {
            // create a unique, non-clustering _id index here.
            BSONObj info = indexInfo(BSON("_id" << 1), true, false, options);
            createIndex(info);
        }
        verify(_idPrimaryKey == idx(findIndexByKeyPattern(BSON("_id" << 1))).clustering());
    }

    IndexedCollection::IndexedCollection(const BSONObj &serialized, bool* reserializeNeeded) :
        CollectionBase(serialized, reserializeNeeded),
        _idPrimaryKey(idx(findIndexByKeyPattern(BSON("_id" << 1))).clustering()) {
    }

    // inserts an object into this collection, taking care of secondary indexes if they exist
    void IndexedCollection::insertObject(BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        obj = addIdField(obj);
        const BSONObj pk = getValidatedPKFromObject(obj);

        // We skip unique checks if the primary key is something other than the _id index.
        // Any other PK is guaranteed to contain the _id somewhere in its pattern, so
        // we know that PK is unique since a unique key on _id must exist.
        insertIntoIndexes(pk, obj, flags | (!_idPrimaryKey ? Collection::NO_PK_UNIQUE_CHECKS : 0), indexBitChanged);
    }

    void IndexedCollection::updateObject(const BSONObj &pk, const BSONObj &oldObj, const BSONObj &newObj,
                                         const bool logop, const bool fromMigrate,
                                         uint64_t flags, bool* indexBitChanged) {
        const BSONObj newObjWithId = inheritIdField(oldObj, newObj);

        if (_idPrimaryKey) {
            CollectionBase::updateObject(pk, oldObj, newObjWithId, logop, fromMigrate, flags | Collection::NO_PK_UNIQUE_CHECKS, indexBitChanged);
        } else {
            const BSONObj newPK = getValidatedPKFromObject(newObjWithId);
            dassert(newPK.nFields() == pk.nFields());
            if (newPK != pk) {
                // Primary key has changed - that means all indexes will be affected.
                deleteFromIndexes(pk, oldObj, flags);
                insertIntoIndexes(newPK, newObjWithId, flags, indexBitChanged);
                if (logop) {
                    OpLogHelpers::logDelete(_ns.c_str(), oldObj, fromMigrate);
                    OpLogHelpers::logInsert(_ns.c_str(), newObjWithId);
                }
            } else {
                // Skip unique checks on the primary key - we know it did not change.
                CollectionBase::updateObject(pk, oldObj, newObjWithId, logop, fromMigrate, flags | Collection::NO_PK_UNIQUE_CHECKS, indexBitChanged);
            }
        }
    }

    // Overridden to optimize the case where we have an _id primary key.
    BSONObj IndexedCollection::getValidatedPKFromObject(const BSONObj &obj) const {
        if (_idPrimaryKey) {
            const BSONElement &e = obj["_id"];
            dassert(e.ok() && e.type() != Array &&
                    e.type() != RegEx && e.type() != Undefined); // already checked in ops/insert.cpp
            return e.wrap("");
        } else {
            return CollectionBase::getValidatedPKFromObject(obj);
        }
    }

    // Overriden to optimize pk generation for an _id primary key.
    // We just need to look for the _id field and, if it exists
    // and is simple, return a wrapped object.
    BSONObj IndexedCollection::getSimplePKFromQuery(const BSONObj &query, const BSONObj& pk) const {
        if (_idPrimaryKey) {
            const BSONElement &e = query["_id"];
            if (e.ok() && e.isSimpleType() &&
                !(e.type() == Object && e.Obj().firstElementFieldName()[0] == '$')) {
                return e.wrap("");
            }
            return BSONObj();
        } else {
            return CollectionBase::getSimplePKFromQuery(query, pk);
        }
    }

    static BSONObj getOplogMinUnsafeKey() {
        if (theReplSet && theReplSet->gtidManager) {
            BSONObjBuilder b;
            GTID minUncommitted = theReplSet->gtidManager->getMinLiveGTID();
            addGTIDToBSON("", minUncommitted, b);
            return b.obj();
        }
        else {
            return minKey;
        }
    }

    // ------------------------------------------------------------------------

    OldOplogCollection::OldOplogCollection(const StringData &ns, const BSONObj &options) :
        IndexedCollection(ns, options) {
        uassert(17206, "must not define a primary key for the oplog",
                       !options["primaryKey"].ok());
    } 

    OldOplogCollection::OldOplogCollection(const BSONObj &serialized) :
        IndexedCollection(serialized) {
    }

    BSONObj OldOplogCollection::minUnsafeKey() {
        return getOplogMinUnsafeKey();
    }

    // ------------------------------------------------------------------------

    OplogPartition::OplogPartition(const StringData &ns, const BSONObj &options) :
        IndexedCollection(ns, options) {
        uassert(0, "must not define a primary key for the oplog",
                       !options["primaryKey"].ok());
    } 

    OplogPartition::OplogPartition(const BSONObj &serialized) :
        IndexedCollection(serialized) {
    }

    BSONObj OplogPartition::minUnsafeKey() {
        return getOplogMinUnsafeKey();
    }

    // ------------------------------------------------------------------------

    PartitionedOplogCollection::PartitionedOplogCollection(const StringData &ns, const BSONObj &options) :
        PartitionedCollection(ns, options) {
        uassert(0, "must not define a primary key for the oplog",
                       !options["primaryKey"].ok());
    } 

    PartitionedOplogCollection::PartitionedOplogCollection(const BSONObj &serialized) :
        PartitionedCollection(serialized) {
    }

    shared_ptr<CollectionData> PartitionedOplogCollection::makeNewPartition(
        const StringData &ns,
        const BSONObj &options
        )
    {
        shared_ptr<OplogPartition> ret;
        ret.reset(new OplogPartition(ns, options));
        return ret;
    }

    // called in constructor
    shared_ptr<CollectionData> PartitionedOplogCollection::openExistingPartition(
        const BSONObj &serialized
        )
    {
        shared_ptr<OplogPartition> ret;
        ret.reset(new OplogPartition(serialized));
        return ret;
    }


    // ------------------------------------------------------------------------

    NaturalOrderCollection::NaturalOrderCollection(const StringData &ns, const BSONObj &options) :
        CollectionBase(ns, BSON("$_" << 1), options),
        _nextPK(0) {
    }

    NaturalOrderCollection::NaturalOrderCollection(const BSONObj &serialized) :
        CollectionBase(serialized),
        _nextPK(0) {
        Client::Transaction txn(DB_TXN_SNAPSHOT | DB_TXN_READ_ONLY);
        {
            // The next PK, if it exists, is the last pk + 1
            shared_ptr<Cursor> cursor = Cursor::make(this, -1, false);
            if (cursor->ok()) {
                const BSONObj key = cursor->currPK();
                dassert(key.nFields() == 1);
                _nextPK = AtomicWord<long long>(key.firstElement().Long() + 1);
            }
        }
        txn.commit();
    }

    // insert an object, using a fresh auto-increment primary key
    void NaturalOrderCollection::insertObject(BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        BSONObjBuilder pk(64);
        pk.append("", _nextPK.fetchAndAdd(1));
        insertIntoIndexes(pk.obj(), obj, flags, indexBitChanged);
    }

    // ------------------------------------------------------------------------

    SystemCatalogCollection::SystemCatalogCollection(const StringData &ns, const BSONObj &options) :
        NaturalOrderCollection(ns, options) {
    }

    SystemCatalogCollection::SystemCatalogCollection(const BSONObj &serialized) :
        NaturalOrderCollection(serialized) {
    }

    void SystemCatalogCollection::insertObject(BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        obj = beautify(obj);
        NaturalOrderCollection::insertObject(obj, flags, indexBitChanged);
    }

    void SystemCatalogCollection::createIndex(const BSONObj &info) {
        msgasserted(16464, "bug: system collections should not be indexed." );
    }

    // For consistency with Vanilla MongoDB, the system catalogs have the following
    // fields, in order, if they exist.
    //
    //  { key, unique, ns, name, [everything else] }
    //
    // This code is largely borrowed from prepareToBuildIndex() in Vanilla.
    BSONObj SystemCatalogCollection::beautify(const BSONObj &obj) {
        BSONObjBuilder b;
        if (obj["key"].ok()) {
            b.append(obj["key"]);
        }
        if (obj["unique"].trueValue()) {
            b.appendBool("unique", true);
        }
        if (obj["ns"].ok()) {
            b.append(obj["ns"]);
        }
        if (obj["name"].ok()) { 
            b.append(obj["name"]);
        }
        for (BSONObjIterator i = obj.begin(); i.more(); ) {
            BSONElement e = i.next();
            string s = e.fieldName();
            if (s != "key" && s != "unique" && s != "ns" && s != "name" && s != "_id") {
                b.append(e);
            }
        }
        return b.obj();
    }

    // ------------------------------------------------------------------------

    BSONObj oldSystemUsersKeyPattern;
    BSONObj extendedSystemUsersKeyPattern;
    std::string extendedSystemUsersIndexName;
    namespace {
        MONGO_INITIALIZER(AuthIndexKeyPatterns)(InitializerContext*) {
            oldSystemUsersKeyPattern = BSON(AuthorizationManager::USER_NAME_FIELD_NAME << 1);
            extendedSystemUsersKeyPattern = BSON(AuthorizationManager::USER_NAME_FIELD_NAME << 1 <<
                                                 AuthorizationManager::USER_SOURCE_FIELD_NAME << 1);
            extendedSystemUsersIndexName = std::string(str::stream() <<
                                                       AuthorizationManager::USER_NAME_FIELD_NAME <<
                                                       "_1_" <<
                                                       AuthorizationManager::USER_SOURCE_FIELD_NAME <<
                                                       "_1");
            return Status::OK();
        }
    }

    BSONObj SystemUsersCollection::extendedSystemUsersIndexInfo(const StringData &ns) {
        BSONObjBuilder indexBuilder;
        indexBuilder.append("key", extendedSystemUsersKeyPattern);
        indexBuilder.appendBool("unique", true);
        indexBuilder.append("ns", ns);
        indexBuilder.append("name", extendedSystemUsersIndexName);
        return indexBuilder.obj();
    }

    SystemUsersCollection::SystemUsersCollection(const StringData &ns, const BSONObj &options) :
        IndexedCollection(ns, options) {
        BSONObj info = extendedSystemUsersIndexInfo(ns);
        createIndex(info);
        uassert(17207, "must not define a primary key for the system.users collection",
                       !options["primaryKey"].ok());
    }

    SystemUsersCollection::SystemUsersCollection(const BSONObj &serialized, bool* reserializeNeeded) :
        IndexedCollection(serialized, reserializeNeeded) {
    }

    // ------------------------------------------------------------------------

    // Capped collections have natural order insert semantics but borrow (ie: copy)
    // its document modification strategy from IndexedCollections. The size
    // and count of a capped collection is maintained in memory and kept valid
    // on txn abort through a CappedCollectionRollback class in the TxnContext. 
    //
    // Tailable cursors over capped collections may only read up to one less
    // than the minimum uncommitted primary key to ensure that they never miss
    // any data. This information is communicated through minUnsafeKey(). On
    // commit/abort, the any primary keys inserted into a capped collection are
    // noted so we can properly maintain the min uncommitted key.
    //
    // In the implementation, NaturalOrderCollection::_nextPK and the set of
    // uncommitted primary keys are protected together by _mutex. Trimming
    // work is done under the _deleteMutex.
    CappedCollection::CappedCollection(const StringData &ns, const BSONObj &options,
                                       const bool mayIndexId) :
        NaturalOrderCollection(ns, options),
        _maxSize(BytesQuantity<long long>(options["size"])),
        _maxObjects(BytesQuantity<long long>(options["max"])),
        _currentObjects(0),
        _currentSize(0),
        _mutex("cappedMutex"),
        _deleteMutex("cappedDeleteMutex") {

        // Create an _id index if "autoIndexId" is missing or it exists as true.
        if (mayIndexId) {
            const BSONElement e = options["autoIndexId"];
            if (!e.ok() || e.trueValue()) {
                BSONObj info = indexInfo(BSON("_id" << 1), true, false, options);
                createIndex(info);
            }
        }
    }
    CappedCollection::CappedCollection(const BSONObj &serialized) :
        NaturalOrderCollection(serialized),
        _maxSize(serialized["options"]["size"].numberLong()),
        _maxObjects(serialized["options"]["max"].numberLong()),
        _currentObjects(0),
        _currentSize(0),
        _mutex("cappedMutex"),
        _deleteMutex("cappedDeleteMutex") {
        
        // Determine the number of objects and the total size.
        // We'll have to look at the data, but this might not be so bad because:
        // - you pay for it once, on initialization.
        // - capped collections are meant to be "small" (fit in memory)
        // - capped collectiosn are meant to be "read heavy",
        //   so bringing it all into memory now helps warmup.
        long long n = 0;
        long long size = 0;
        Client::Transaction txn(DB_TXN_SNAPSHOT | DB_TXN_READ_ONLY);
        for (shared_ptr<Cursor> c( Cursor::make(this, 1, false) ); c->ok(); n++, c->advance()) {
            size += c->current().objsize();
        }
        txn.commit();

        _currentObjects = AtomicWord<long long>(n);
        _currentSize = AtomicWord<long long>(size);
        verify((_currentSize.load() > 0) == (_currentObjects.load() > 0));
    }

    void CappedCollection::fillSpecificStats(BSONObjBuilder &result, int scale) const {
        result.appendBool("capped", true);
        if (_maxObjects) {
            result.appendNumber("max", _maxObjects);
        }
        result.appendNumber("cappedCount", _currentObjects.load());
        result.appendNumber("cappedSizeMax", _maxSize);
        result.appendNumber("cappedSizeCurrent", _currentSize.load());
    }

    // @return the maximum safe key to read for a tailable cursor.
    BSONObj CappedCollection::minUnsafeKey() {
        SimpleMutex::scoped_lock lk(_mutex);

        const long long minUncommitted = _uncommittedMinPKs.size() > 0 ?
                                         _uncommittedMinPKs.begin()->firstElement().Long() :
                                         _nextPK.load();
        TOKULOG(2) << "minUnsafeKey: minUncommitted " << minUncommitted << endl;
        BSONObjBuilder b;
        b.append("", minUncommitted);
        return b.obj();
    }

    // run an insertion where the PK is specified
    // Can come from the applier thread on a slave or a cloner 
    void CappedCollection::insertObjectWithPK(const BSONObj &pk, const BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        SimpleMutex::scoped_lock lk(_mutex);
        long long pkVal = pk[""].Long();
        if (pkVal >= _nextPK.load()) {
            _nextPK = AtomicWord<long long>(pkVal + 1);
        }

        // Must note the uncommitted PK before we do the actual insert,
        // since we check the capped rollback data structure to see if
        // any inserts have happened yet for this transaction (and that
        // would erroneously be true if we did the insert here first).
        noteUncommittedPK(pk);
        checkUniqueAndInsert(pk, obj, flags, true, indexBitChanged);

    }

    void CappedCollection::insertObjectAndLogOps(const BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        const BSONObj objWithId = addIdField(obj);
        uassert( 16774 , str::stream() << "document is larger than capped size "
                 << objWithId.objsize() << " > " << _maxSize, objWithId.objsize() <= _maxSize );

        const BSONObj pk = getNextPK();
        checkUniqueAndInsert(pk, objWithId, flags | Collection::NO_UNIQUE_CHECKS | Collection::NO_LOCKTREE, false, indexBitChanged);
        OpLogHelpers::logInsertForCapped(_ns.c_str(), pk, objWithId);
        checkGorged(obj, true);
    }

    void CappedCollection::insertObject(BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        obj = addIdField(obj);
        _insertObject(obj, flags, indexBitChanged);
    }

    void CappedCollection::deleteObject(const BSONObj &pk, const BSONObj &obj, uint64_t flags) {
        msgasserted(16460, "bug: cannot remove from a capped collection, "
                           " should have been enforced higher in the stack" );
    }

    // run a deletion where the PK is specified
    // Can come from the applier thread on a slave
    void CappedCollection::deleteObjectWithPK(const BSONObj &pk, const BSONObj &obj, uint64_t flags) {
        _deleteObject(pk, obj, flags);
        // just make it easy and invalidate this
        _lastDeletedPK = BSONObj();
    }

    void CappedCollection::updateObject(const BSONObj &pk, const BSONObj &oldObj, const BSONObj &newObj,
                                        const bool logop, const bool fromMigrate,
                                        uint64_t flags, bool* indexBitChanged) {
        const BSONObj newObjWithId = inheritIdField(oldObj, newObj);
        long long diff = newObjWithId.objsize() - oldObj.objsize();
        uassert( 10003 , "failing update: objects in a capped ns cannot grow", diff <= 0 );

        CollectionBase::updateObject(pk, oldObj, newObjWithId, logop, fromMigrate, flags, indexBitChanged);
        if (diff < 0) {
            _currentSize.addAndFetch(diff);
        }
    }

    void CappedCollection::updateObjectMods(const BSONObj &pk, const BSONObj &updateobj,
                                            const bool logop, const bool fromMigrate,
                                            uint64_t flags) {
        msgasserted(17217, "bug: cannot (fast) update a capped collection, "
                           " should have been enforced higher in the stack" );
    }

    void CappedCollection::_insertObject(const BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        uassert( 16328 , str::stream() << "document is larger than capped size "
                 << obj.objsize() << " > " << _maxSize, obj.objsize() <= _maxSize );

        const BSONObj pk = getNextPK();
        checkUniqueAndInsert(pk, obj, flags | Collection::NO_UNIQUE_CHECKS | Collection::NO_LOCKTREE, false, indexBitChanged);
        checkGorged(obj, false);
    }

    // Note the commit of a transaction, which simple notes completion under the lock.
    // We don't need to do anything with nDelta and sizeDelta because those changes
    // are already applied to in-memory stats, and this transaction has committed.
    void CappedCollection::noteCommit(const BSONObj &minPK, long long nDelta, long long sizeDelta) {
        noteComplete(minPK);
    }

    // Note the abort of a transaction, noting completion and updating in-memory stats.
    //
    // The given deltas are signed values that represent changes to the collection.
    // We need to roll back those changes. Therefore, we subtract from the current value.
    void CappedCollection::noteAbort(const BSONObj &minPK, long long nDelta, long long sizeDelta) {
        noteComplete(minPK);
        _currentObjects.fetchAndSubtract(nDelta);
        _currentSize.fetchAndSubtract(sizeDelta);

        // If this transaction did inserts, it probably did deletes to make room
        // for the new objects. Invalidate the last key deleted so that new
        // trimming work properly recognizes that our deletes have been aborted.
        SimpleMutex::scoped_lock lk(_deleteMutex);
        _lastDeletedPK = BSONObj();
    }

    // requires: _mutex is held
    void CappedCollection::noteUncommittedPK(const BSONObj &pk) {
        CappedCollectionRollback &rollback = cc().txn().cappedRollback();
        if (!rollback.hasNotedInsert(_ns)) {
            // This transaction has not noted an insert yet, so we save this
            // as a minimum uncommitted PK. The next insert by this txn won't be
            // the minimum, and rollback.hasNotedInsert() will be true, so
            // we won't save it.
            _uncommittedMinPKs.insert(pk.getOwned());
        }
    }

    BSONObj CappedCollection::getNextPK() {
        SimpleMutex::scoped_lock lk(_mutex);
        BSONObjBuilder b(32);
        b.append("", _nextPK.fetchAndAdd(1));
        BSONObj pk = b.obj();
        noteUncommittedPK(pk);
        return pk;
    }

    // Note the completion of a transaction by removing its
    // minimum-PK-inserted (if there is one) from the set.
    void CappedCollection::noteComplete(const BSONObj &minPK) {
        if (!minPK.isEmpty()) {
            SimpleMutex::scoped_lock lk(_mutex);
            const int n = _uncommittedMinPKs.erase(minPK);
            verify(n == 1);
        }
    }

    void CappedCollection::checkGorged(const BSONObj &obj, bool logop) {
        // If the collection is gorged, we need to do some trimming work.
        long long n = _currentObjects.load();
        long long size = _currentSize.load();
        if (isGorged(n, size)) {
            trim(obj.objsize(), logop);
        }
    }

    void CappedCollection::checkUniqueIndexes(const BSONObj &pk, const BSONObj &obj, bool checkPk) {
        dassert(!pk.isEmpty());
        dassert(!obj.isEmpty());

        // Start at 1 to skip the primary key index. We don't need to perform
        // a unique check because we always generate a unique auto-increment pk.
        int start = checkPk ? 0 : 1;
        for (int i = start; i < nIndexes(); i++) {
            IndexDetailsBase &idx = *_indexes[i];
            if (idx.unique()) {
                BSONObjSet keys;
                idx.getKeysFromObject(obj, keys);
                for (BSONObjSet::const_iterator ki = keys.begin(); ki != keys.end(); ++ki) {
                    idx.uniqueCheck(*ki, pk);
                }
            }
        }
    }

    // Checks unique indexes and does the actual inserts.
    // Does not check if the collection became gorged.
    void CappedCollection::checkUniqueAndInsert(const BSONObj &pk, const BSONObj &obj, uint64_t flags, bool checkPk, bool* indexBitChanged) {
        // Note the insert we're about to do.
        CappedCollectionRollback &rollback = cc().txn().cappedRollback();
        rollback.noteInsert(_ns, pk, obj.objsize());
        _currentObjects.addAndFetch(1);
        _currentSize.addAndFetch(obj.objsize());

        checkUniqueIndexes(pk, obj, checkPk);

        // The actual insert should not hold take any locks and does
        // not need unique checks, since we generated a unique primary
        // key and checked for uniquness constraints on secondaries above.
        insertIntoIndexes(pk, obj, flags, indexBitChanged);
    }

    bool CappedCollection::isGorged(long long n, long long size) const {
        return (_maxObjects > 0 && n > _maxObjects) || (_maxSize > 0 && size > _maxSize);
    }

    void CappedCollection::_deleteObject(const BSONObj &pk, const BSONObj &obj, uint64_t flags) {
        // Note the delete we're about to do.
        size_t size = obj.objsize();
        CappedCollectionRollback &rollback = cc().txn().cappedRollback();
        rollback.noteDelete(_ns, pk, size);
        _currentObjects.subtractAndFetch(1);
        _currentSize.subtractAndFetch(size);

        NaturalOrderCollection::deleteObject(pk, obj, flags);
    }

    void CappedCollection::trim(int objsize, bool logop) {
        SimpleMutex::scoped_lock lk(_deleteMutex);
        long long n = _currentObjects.load();
        long long size = _currentSize.load();
        if (isGorged(n, size)) {
            // Delete older objects until we've made enough room for the new one.
            // If other threads are trying to insert concurrently, we will do some
            // work on their behalf (until !isGorged). But we stop if we've deleted
            // K objects and done enough to satisfy our own intent, to limit latency.
            const int K = 8;
            int trimmedBytes = 0;
            int trimmedObjects = 0;
            const long long startKey = !_lastDeletedPK.isEmpty() ?
                                       _lastDeletedPK.firstElement().Long() : 0;
            // TODO: Disable prelocking on this cursor, or somehow prevent waiting 
            //       on row locks we can't get immediately.
            for (shared_ptr<Cursor> c(Cursor::make(this, getPKIndex(), // pk always has multiKey false
                                      BSON("" << startKey), maxKey, true, 1, 0, false));
                 c->ok(); c->advance()) {
                BSONObj oldestPK = c->currPK();
                BSONObj oldestObj = c->current();
                trimmedBytes += oldestPK.objsize();
                
                if (logop) {
                    OpLogHelpers::logDeleteForCapped(_ns.c_str(), oldestPK, oldestObj);
                }
                
                // Delete the object, reload the current objects/size
                _deleteObject(oldestPK, oldestObj, 0);
                _lastDeletedPK = oldestPK.getOwned();
                n = _currentObjects.load();
                size = _currentSize.load();
                trimmedObjects++;

                if (!isGorged(n, size) || (trimmedBytes >= objsize && trimmedObjects >= K)) {
                    break;
                }
            }
        }
    }

    // Remove everything from this capped collection
    void CappedCollection::empty() {
        SimpleMutex::scoped_lock lk(_deleteMutex);
        for (shared_ptr<Cursor> c( Cursor::make(this, 1, false) ); c->ok() ; c->advance()) {
            _deleteObject(c->currPK(), c->current(), 0);
        }
        _lastDeletedPK = BSONObj();
    }

    // ------------------------------------------------------------------------

    ProfileCollection::ProfileCollection(const StringData &ns, const BSONObj &options) :
        // Never automatically index the _id field
        CappedCollection(ns, options, false) {
    }

    ProfileCollection::ProfileCollection(const BSONObj &serialized) :
        CappedCollection(serialized) {
    }

    void ProfileCollection::insertObject(BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        _insertObject(obj, flags, indexBitChanged);
    }

    void ProfileCollection::updateObject(const BSONObj &pk, const BSONObj &oldObj, const BSONObj &newObj,
                                         const bool logop, const bool fromMigrate,
                                         uint64_t flags, bool* indexBitChanged) {
        msgasserted( 16850, "bug: The profile collection should not be updated." );
    }

    void ProfileCollection::updateObjectMods(const BSONObj &pk, const BSONObj &updateobj,
                                             const bool logop, const bool fromMigrate,
                                             uint64_t flags) {
        msgasserted( 17219, "bug: The profile collection should not be updated." );
    }

    void ProfileCollection::createIndex(const BSONObj &idx_info) {
        uassert(16851, "Cannot have an _id index on the system profile collection", !idx_info["key"]["_id"].ok());
    }

    // ------------------------------------------------------------------------

    BulkLoadedCollection::BulkLoadedCollection(const BSONObj &serialized) :
        IndexedCollection(serialized),
        _bulkLoadConnectionId(cc().getConnectionId()) {
        // By noting this ns in the collection map rollback, we will automatically
        // abort the load if the calling transaction aborts, because close()
        // will be called with aborting = true. See BulkLoadedCollection::close()
        CollectionMapRollback &rollback = cc().txn().collectionMapRollback();
        rollback.noteNs(_ns);

        const int n = _nIndexes;
        _dbs.reset(new DB *[n]);
        _multiKeyTrackers.reset(new scoped_ptr<MultiKeyTracker>[n]);

        for (int i = 0; i < _nIndexes; i++) {
            IndexDetailsBase &idx = *_indexes[i];
            _dbs[i] = idx.db();
            _multiKeyTrackers[i].reset(new MultiKeyTracker(_dbs[i]));
        }
        _loader.reset(new storage::Loader(_dbs.get(), n));
        _loader->setPollMessagePrefix(str::stream() << "Loader build progress: " << _ns);
    }

    void BulkLoadedCollection::close(const bool abortingLoad, bool* indexBitsChanged) {
        *indexBitsChanged = false;
        class FinallyClose : boost::noncopyable {
        public:
            FinallyClose(BulkLoadedCollection &coll, bool abort) : _c(coll), _abortingLoad(abort) {}
            ~FinallyClose() {
                bool indexBitsChanged= false;
                _c._close(_abortingLoad, &indexBitsChanged);
                verify(!indexBitsChanged); // this should not be true
            }
        private:
            BulkLoadedCollection &_c;
            bool _abortingLoad;
        } finallyClose(*this, abortingLoad);

        if (!abortingLoad) {
            const int r = _loader->close();
            if (r != 0) {
                storage::handle_ydb_error(r);
            }
            verify(!_indexBuildInProgress);
            for (int i = 0; i < _nIndexes; i++) {
                IndexDetailsBase &idx = *_indexes[i];
                // The PK's uniqueness is verified on loader close, so we should not check it again.
                if (!isPKIndex(idx) && idx.unique()) {
                    checkIndexUniqueness(idx);
                }
                if (_multiKeyTrackers[i]->isMultiKey()) {
                    setIndexIsMultikey(i, indexBitsChanged);
                }
            }
        }
    }

    void BulkLoadedCollection::validateConnectionId(const ConnectionId &id) {
        uassert( 16878, str::stream() << "This connection cannot use ns " << _ns <<
                        ", it is currently under-going bulk load by connection id "
                        << _bulkLoadConnectionId,
                        _bulkLoadConnectionId == id );
    }

    void BulkLoadedCollection::insertObject(BSONObj &obj, uint64_t flags, bool* indexBitChanged) {
        obj = addIdField(obj);
        const BSONObj pk = getValidatedPKFromObject(obj);

        storage::Key sPK(pk, NULL);
        DBT key = storage::dbt_make(sPK.buf(), sPK.size());
        DBT val = storage::dbt_make(obj.objdata(), obj.objsize());
        _loader->put(&key, &val);
        // multiKey stuff taken care of during close(), so indexBitChanged is not set
    }

    void BulkLoadedCollection::deleteObject(const BSONObj &pk, const BSONObj &obj, uint64_t flags) {
        uasserted( 16865, "Cannot delete from a collection under-going bulk load." );
    }

    void BulkLoadedCollection::updateObject(const BSONObj &pk, const BSONObj &oldObj, const BSONObj &newObj,
                                            const bool logop, const bool fromMigrate,
                                            uint64_t flags, bool* indexBitChanged) {
        uasserted( 16866, "Cannot update a collection under-going bulk load." );
    }

    void BulkLoadedCollection::updateObjectMods(const BSONObj &pk, const BSONObj &updateobj,
                                                const bool logop, const bool fromMigrate,
                                                uint64_t flags) {
        uasserted( 17218, "Cannot update a collection under-going bulk load." );
    }

    bool BulkLoadedCollection::rebuildIndex(int i, const BSONObj &options, BSONObjBuilder &wasBuilder) {
        uasserted( 16895, "Cannot optimize a collection under-going bulk load." );
    }

    void BulkLoadedCollection::dropIndexDetails(const int idxNum) {
        uasserted( 16894, "Cannot perform drop/dropIndexes on of a collection under-going bulk load." );
    }

    // When closing a BulkLoadedCollection, we need to make sure the key trackers and
    // loaders are destructed before we call up to the parent destructor, because they
    // reference storage::Dictionaries that get destroyed in the parent destructor.
    void BulkLoadedCollection::_close(bool aborting, bool* indexBitsChanged) {
        _loader.reset();
        _multiKeyTrackers.reset();
        CollectionBase::close(aborting, indexBitsChanged);
    }

    void BulkLoadedCollection::createIndex(const BSONObj &info) {
        uasserted( 16867, "Cannot create an index on a collection under-going bulk load." );
    }

    //
    // methods for PartitionedCollections
    //
    
    static BSONObj makePartitionedSerialized(BSONObj serialized, string newNS) {
        BSONArrayBuilder newIndexesArray;
        vector<BSONElement> indexes = serialized["indexes"].Array();
        for (vector<BSONElement>::const_iterator it = indexes.begin(); it != indexes.end(); it++) {
            newIndexesArray.append(replaceNSField(it->Obj(), newNS));
        }
        return Collection::serialize(newNS, serialized["options"].Obj(), serialized["pk"].Obj(),
                                                          serialized["multiKeyIndexBits"].Long(),
                                                          newIndexesArray.arr());
    }

    // constructor that creates a PartitionedCollection
    PartitionedCollection::PartitionedCollection(const StringData &ns, const BSONObj &options) :
        CollectionData(ns, BSON("_id" << 1)), // the pk MUST be _id:1, we verify below
        _options(options.copy()),
        _ordering(Ordering::make(BSONObj())), // dummy for now, we create it properly below
        _numPartitions(0)
    {
        // MUST CALL initialize directly after this.
        // We can't call initialize here because it depends on virtual functions
    }

    void PartitionedCollection::initialize(const StringData &ns, const BSONObj &options) {
        // verify that there is no defined primary key
        uassert(17245, str::stream() << "Partitioned Collection cannot have a defined primary key: " << ns, !options["primaryKey"].ok());

        // create the meta collection
        _metaCollection.reset(new IndexedCollection(getMetaCollectionName(ns), options));

        // add the first partition
        appendNewPartition();

        // some sanity checks
        verify(_numPartitions == 1);
        verify(_partitions[0]->nIndexes() == 1);

        // create the index details
        createIndexDetails();
        
        sanityCheck();    
    }

    PartitionedCollection::PartitionedCollection(const BSONObj &serialized) :
        CollectionData(serialized),
        _options(serialized["options"].Obj().copy()),
        _ordering(Ordering::make(BSONObj())), // dummy for now, we create it properly below
        _numPartitions(0)
    {
        // MUST CALL initialize directly after this.
        // We can't call initialize here because it depends on virtual functions
    }

    void PartitionedCollection::initialize(const BSONObj &serialized) {
        // verify that there is no defined primary key
        uassert(17246, str::stream() << "Partitioned Collection cannot have a defined primary key: " << _ns, !_options["primaryKey"].ok());
        // open the metadata collection
        BSONObj metaSerialized = makePartitionedSerialized(serialized, getMetaCollectionName(_ns));
        _metaCollection.reset(new IndexedCollection(metaSerialized));
        // now we need to query _metaCollection to get partition information
        for (shared_ptr<Cursor> c( Cursor::make(_metaCollection.get(), 1, false) ); c->ok() ; c->advance()) {
            BSONObj curr = c->current();
            TOKULOG(1) << "found" << curr.str() << " in meta collection" << endl;
            // All ids that are numbers in the metadata collection correspond to numbers
            // otherwise, they may be something else (anything else, as who knows what
            // the future holds). So, if we see an id that is not a number, we continue
            if (!curr["_id"].isNumber()) {
                TOKULOG(1) << "continuing on " << curr.str() << endl;
                continue;
            }
            // make the collection
            uint64_t currID = curr["_id"].numberLong();
            BSONObj currCollSerialized = makePartitionedSerialized(
                serialized,
                getPartitionName(currID)
                );
            _partitions.push_back(openExistingPartition(currCollSerialized));
            // extract the pivot
            _partitionPivots.push_back(curr["max"].Obj().copy());
            _partitionIDs.push_back(currID);
            
            _numPartitions++;
        }
        // create the index details
        createIndexDetails();
        
        sanityCheck();    
    }

    shared_ptr<CollectionData> PartitionedCollection::makeNewPartition(
        const StringData &ns,
        const BSONObj &options
        )
    {
        shared_ptr<IndexedCollection> ret;
        ret.reset(new IndexedCollection(ns, options));
        return ret;
    }

    // called in constructor
    shared_ptr<CollectionData> PartitionedCollection::openExistingPartition(
        const BSONObj &serialized
        )
    {
        shared_ptr<IndexedCollection> ret;
        ret.reset(new IndexedCollection(serialized));
        return ret;
    }

    shared_ptr<Cursor> PartitionedCollection::makeCursor(
        const int direction, 
        const bool countCursor
        )
    {
        shared_ptr<Cursor> ret(new PartitionedCursor(this, direction, countCursor));
        return ret;
    }
    
    // index range scan between start/end
    shared_ptr<Cursor> PartitionedCollection::makeCursor(const IndexDetails &idx,
                                   const BSONObj &startKey, const BSONObj &endKey,
                                   const bool endKeyInclusive,
                                   const int direction, const int numWanted,
                                   const bool countCursor)
    {
        // as of now, no secondary indexes
        verify(idxNo(idx) == 0);
        shared_ptr<Cursor> ret(
            new PartitionedCursor(
                this,
                startKey,
                endKey,
                endKeyInclusive,
                direction,
                numWanted,
                countCursor
                )
            );
        return ret;
    }
    
    // index range scan by field bounds
    shared_ptr<Cursor> PartitionedCollection::makeCursor(const IndexDetails &idx,
                                   const shared_ptr<FieldRangeVector> &bounds,
                                   const int singleIntervalLimit,
                                   const int direction, const int numWanted,
                                   const bool countCursor)
    {
        // as of now, no secondary indexes
        verify(idxNo(idx) == 0);
        shared_ptr<Cursor> ret(
            new PartitionedCursor (
                this,
                bounds,
                singleIntervalLimit,
                direction,
                numWanted,
                countCursor
                )
            );
        return ret;
    }

    void PartitionedCollection::sanityCheck() {
        verify(_numPartitions == _partitions.size());
        verify(_numPartitions == _partitionPivots.size());
        verify(_numPartitions == _partitionIDs.size());
        // verify that the last pivot is maxKey
        verify( _partitionPivots[_numPartitions - 1][""].type() == MaxKey );
        // verify that pivots are increasing in order
        for (uint64_t i = 1; i < _numPartitions; i++) {
            BSONObj bigger = _partitionPivots[i];
            BSONObj smaller = _partitionPivots[i-1];
            verify(bigger.woCompare(smaller, _ordering) > 0);
        }

        uint64_t currIdx = 0;
        for (shared_ptr<Cursor> c( Cursor::make(_metaCollection.get(), 1, false) ); c->ok() ; c->advance()) {
            BSONObj curr = c->current();
            // All ids that are numbers in the metadata collection correspond to numbers
            // otherwise, they may be something else (anything else, as who knows what
            // the future holds). So, if we see an id that is not a number, we continue
            if (!curr["_id"].isNumber()) {
                continue;
            }
            // make the collection
            uint64_t currID = curr["_id"].numberLong();
            verify(currID == _partitionIDs[currIdx]);
            verify(_partitionPivots[currIdx].woCompare(curr["max"].Obj(), _ordering) == 0);
            currIdx++;
        }
    }

    // called by constructors during initialization. Must be done
    // after meta collection and partitions instantiated
    void PartitionedCollection::createIndexDetails() {
        shared_ptr<PartitionedIndexDetails> details;
        BSONObj info = _partitions[0]->idx(0).info();
        details.reset(
            new PartitionedIndexDetails(
                replaceNSField(info, _ns),
                this,
                0
                )
            );
        _indexDetails.push_back(details);
        // initialize _ordering
        _ordering = Ordering::make(details->keyPattern());
    }

    // helper function for add partition
    void PartitionedCollection::capLastPartition() {
        BSONObj currPK;
        // in the last partition, look up the max value,
        // make a reverse cursor on the last partition
        shared_ptr<Cursor> c( Cursor::make(_partitions[_numPartitions-1].get(), -1, false));
        // if there is nothing in the partition, then we cannot cap it
        // so throw an error. This may happen if a user tries to create
        // a partition on top of an empty one. We disallow this because
        // having multiple partitions with the same max key can
        // lead to bugs.
        uassert(17248, "can only cap a partition with no pivot if it is non-empty", c->ok());        
        currPK = c->currPK();
        // make the max PK new pivot
        overwritePivot(_numPartitions-1, currPK);
    }

    // case where user manually passes is what they want the capped value
    // to be
    void PartitionedCollection::manuallyCapLastPartition(BSONObj newPivot) {
        // first, make sure pivot we want to add must be greater
        // than its last pivot
        if (_numPartitions > 1) {
            uassert(
                17249, 
                str::stream() <<"new pivot must be greater than last pivot, newPivot: " << newPivot.str() << 
                " lastPivot: " <<_partitionPivots[_numPartitions-2].str(), 
                newPivot.woCompare(_partitionPivots[_numPartitions-2], _ordering) > 0
                );
        }
        // in the last partition, look up the max value,
        // make a reverse cursor on the last partition
        shared_ptr<Cursor> c( Cursor::make(_partitions[_numPartitions-1].get(), -1, false));
        if (c->ok()) {
            BSONObj currPK = c->currPK();
            // make sure that the pivot we want to add
            // is greater than the maximum value we see
            // in the partition (hence a valid pivot)
            uassert(
                17250, 
                str::stream() << "new pivot must be greater than max element in collection, newPivot: " <<
                newPivot.str() << " max element: " << currPK.str(), 
                newPivot.woCompare(currPK, _ordering) > 0);
        }
        overwritePivot(_numPartitions-1, newPivot);
    }

    // for pivot associated with ith partition (note, not the id),
    // replace the pivot with newPivot, both in _metaCollection
    // and in _partitionPivots
    void PartitionedCollection::overwritePivot(uint64_t i, BSONObj newPivot) {
        uint64_t id = _partitionIDs[i];
        BSONObj result;
        bool idExists = _metaCollection->findByPK(BSON("" << id), result);
        uassert(17251, str::stream() << "could not find partition " << id << " of ns " << _ns, idExists);

        // now we need to replace the max entry with newPivot
        BSONObjBuilder b;
        BSONObjIterator ii( result );
        while ( ii.more() ) {
            BSONElement e = ii.next();
            if ( strcmp( e.fieldName(), "max" ) != 0 ) {
                b.append( e );
            } else {
                b.append("max", newPivot);
            }
        }
        bool indexBitChanged = false;
        BSONObj pk = _metaCollection->getValidatedPKFromObject(result);
        _metaCollection->updateObject(pk, result, b.done(),
                                            false, false,
                                            0, &indexBitChanged);
        verify(!indexBitChanged);

        // at this point, _metaCollection should be updated, now update
        // in memory stuff
        _partitionPivots[i] = newPivot.copy();
    }

    void PartitionedCollection::appendPartition(BSONObj partitionInfo) {
        uint64_t id = partitionInfo["_id"].numberLong();
        bool indexBitChanged = false;
        _metaCollection->insertObject(partitionInfo, 0, &indexBitChanged);
        verify(!indexBitChanged); // for sanity

        // add new collection to vector of partitions
        shared_ptr<CollectionData> newPartition;
        newPartition = makeNewPartition(getPartitionName(id), _options);
        _partitions.push_back(newPartition);

        // add data to internal vectors
        _partitionPivots.push_back(partitionInfo["max"].Obj().copy());
        _partitionIDs.push_back(id);
        _numPartitions++;

        // some sanity checks
        verify(_partitions.size() == _numPartitions);
        verify(_partitions[_numPartitions-1].get() == newPartition.get());    
    }
    
    void PartitionedCollection::appendNewPartition() {
        Lock::assertWriteLocked(_ns);
        uint64_t id = (_numPartitions == 0) ? 0 : _partitionIDs[_numPartitions-1] + 1;

        // make entry for metadata collection
        // the pivot
        BSONObjBuilder c(64);
        c.appendMaxKey("");
        // doc to be inserted into meta collection
        BSONObjBuilder b(64);
        b.append("_id", (long long)id);
        b.append("max", c.done());
        b.appendDate("createTime", curTimeMillis64());

        // write to metadata collection
        appendPartition(b.obj());

        sanityCheck();
    }

    // create partitions from the cloner
    void PartitionedCollection::addClonedPartitionInfo(vector<BSONElement> partitionInfo) {
        uassert(0, "Called addClonedPartitionInfo with more than one current partition", (_numPartitions == 1));
        // Note that the caller of this function needs to be REALLY careful
        // we don't do a lot of sanity checks that we theoretically could do,
        // such as "the collection has one partition and is empty"
        // we assume this is being called right after userCreateNS and within the cloner

        // just a simple sanity check
        uassert(0, "sanity check if id of first partition failed", (_partitionIDs[0] == 0));
        
        // first drop the existing partition.
        dropPartitionInternal(_partitionIDs[0]);
        // now add the rest of the partitions        
        for (vector<BSONElement>::const_iterator it = partitionInfo.begin(); it != partitionInfo.end(); it++) {
            appendPartition(it->Obj());
        }
        sanityCheck();
    }

    void PartitionedCollection::addPartition() {
        Lock::assertWriteLocked(_ns);
        sanityCheck();
        // ensure only one thread can modify partitions at a time
        
        CollectionMapRollback &rollback = cc().txn().collectionMapRollback();
        rollback.noteNs(_ns);

        // if we have multiple multi-statement transactions adding partitions
        // concurrently, things can get hairy (like closing during an abort).
        // So, to keep this simple, only allowing one transaction the ability
        // to drop or add a collection at a time
        _metaCollection->acquireTableLock();
        // kill all cursors first, this may be unnecessary, determine later
        ClientCursor::invalidate(_ns);
        capLastPartition();
        appendNewPartition();
        sanityCheck();
    }

    void PartitionedCollection::manuallyAddPartition(BSONObj newPivot) {
        Lock::assertWriteLocked(_ns);
        sanityCheck();

        CollectionMapRollback &rollback = cc().txn().collectionMapRollback();
        rollback.noteNs(_ns);

        // ensure only one thread can modify partitions at a time
        // if we have multiple multi-statement transactions adding partitions
        // concurrently, things can get hairy (like closing during an abort).
        // So, to keep this simple, only allowing one transaction the ability
        // to drop or add a collection at a time
        _metaCollection->acquireTableLock();
        // kill all cursors first, this may be unnecessary, determine later
        ClientCursor::invalidate(_ns);
        manuallyCapLastPartition(getValidatedPKFromObject(newPivot));
        appendNewPartition();
        sanityCheck();
    }

    void PartitionedCollection::getPartitionInfo(uint64_t* numPartitions, BSONArray &partitionArray) {
        *numPartitions = _numPartitions;
        BSONArrayBuilder b;
        // for sanity checking
        uint64_t numPartitionsFoundInMeta = 0;
        for (shared_ptr<Cursor> c( Cursor::make(_metaCollection.get(), 1, false) ); c->ok() ; c->advance()) {
            BSONObj curr = c->current();
            // All ids that are numbers in the metadata collection correspond to numbers
            // otherwise, they may be something else (anything else, as who knows what
            // the future holds). So, if we see an id that is not a number, we continue
            if (!curr["_id"].isNumber()) {
                continue;
            }
            b.append(curr);
            numPartitionsFoundInMeta++;
        }
        partitionArray = b.arr();
        verify(numPartitionsFoundInMeta == _numPartitions);
    }

    uint64_t PartitionedCollection::findInMemoryPartition(uint64_t id) {
        // now find the index of the in-memory vectors associated with this
        // item
        uint64_t high = _numPartitions - 1;
        uint64_t low = 0;
        while (low < high) {
            uint64_t mid = (high + low) / 2;
            uint64_t currID = _partitionIDs[mid];
            if (currID == id) {
                return mid;
            }
            else if (currID < id) {
                low = mid+1;
            }
            else {
                high = mid;
            }
        }
        return low;
    }

    void PartitionedCollection::dropPartitionInternal(uint64_t id) {
        Lock::assertWriteLocked(_ns);
        CollectionMapRollback &rollback = cc().txn().collectionMapRollback();
        rollback.noteNs(_ns);

        // ensure only one thread can modify partitions at a time
        // if we have multiple multi-statement transactions adding partitions
        // concurrently, things can get hairy (like closing during an abort).
        // So, to keep this simple, only allowing one transaction the ability
        // to drop or add a collection at a time
        _metaCollection->acquireTableLock();

        // kill all cursors first
        ClientCursor::invalidate(_ns);

        BSONObjBuilder b(64);
        b.append("", id);
        BSONObj pk = b.done();
        BSONObj result;
        bool idExists = _metaCollection->findByPK(b.done(), result);
        uassert(17253, str::stream() << "could not find partition " << id << " of ns " << _ns, idExists);

        // first delete entry from _metaCollection
        _metaCollection->deleteObject(pk, result, 0);
        uint64_t index = findInMemoryPartition(id);
        // something very wrong if this is false, but sanity check
        // done at top should ensure this never happens
        verify(_partitionIDs[index] == id);

        // now that we have index, clean up in-memory data structures
        _partitionPivots.erase(_partitionPivots.begin() + index);
        _partitionIDs.erase(_partitionIDs.begin() + index);
        // ugly way to "drop" a collection. Perhaps we need
        // a CollectionData method for this.
        for (int i = 0; i < nIndexes(); i++) {
            _partitions[index]->dropIndexDetails(i);
        }
        _partitions.erase(_partitions.begin() + index);
        
        // special case for dropping last partition, we have to fix up
        // the last pivot
        // note that _numPartitions may be 1 if called by addClonedPartitionInfo
        // in that case, we don't want to do this
        if ((index == _numPartitions - 1) && _numPartitions > 1) {
            BSONObjBuilder c(64);
            c.appendMaxKey("");
            overwritePivot(_numPartitions - 2, c.done());
        }

        _numPartitions--;
    }

    // this is called by the user
    void PartitionedCollection::dropPartition(uint64_t id) {
        sanityCheck();        
        uassert(17252, "cannot drop partition if only one exists", _numPartitions > 1);
        dropPartitionInternal(id);
        sanityCheck();
    }
    
    int PartitionedCollection::partitionWithPK(const BSONObj& pk) const {
        // if there is one partition, then the answer is easy
        if (_numPartitions == 1) {
            return 0;
        }
        // first check the last partition, as we expect many inserts and
        //queries to go there
        if (pk.woCompare(_partitionPivots[_numPartitions-2], _ordering) > 0) {
            return _numPartitions - 1;
        }
        // It is not in the last partition, now we must do a binary search
        uint64_t high = _numPartitions - 2;
        uint64_t low = 0;
        while (low < high) {
            uint64_t mid = (high + low) / 2;
            int cmp = pk.woCompare(_partitionPivots[mid], _ordering);
            if (cmp == 0) {
                // we are right at a boundary, we know the result
                // return it
                return mid;
            }
            else if (cmp < 0 ) {
                // too high
                high = mid;
            }
            else {
                // too low
                low = mid+1;
            }
        }
        verify (low == high);
        return low;
    }

    string PartitionedCollection::getMetaCollectionName(const StringData &ns) {
        mongo::StackStringBuilder ss;
        ss << ns << "$$meta";
        return ss.str();
    }

    string PartitionedCollection::getPartitionName(uint64_t partitionID) {
        mongo::StackStringBuilder ss;
        ss << _ns << "$$p" << partitionID;
        return ss.str();
    }
    
    void PartitionedCollection::updateObject(const BSONObj &pk, const BSONObj &oldObj, const BSONObj &newObj,
                              const bool logop, const bool fromMigrate,
                              uint64_t flags, bool* indexBitChanged)
    {
        int whichPartition = partitionWithPK(pk);
        // note we are passing false for logop
        _partitions[whichPartition]->updateObject(pk, oldObj, newObj, false, fromMigrate, flags, indexBitChanged);
        if (logop) {
            OpLogHelpers::logUpdate(_ns.c_str(), pk, oldObj, newObj, fromMigrate);
        }
    }

} // namespace mongo
