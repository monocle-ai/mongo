//@file update.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "pch.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/oplog.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_internal.h"
#include "mongo/db/oplog_helpers.h"

namespace mongo {

    void updateOneObject(
        NamespaceDetails *d, 
        NamespaceDetailsTransient *nsdt, 
        const BSONObj &pk, 
        const BSONObj &oldObj, 
        const BSONObj &newObj, 
        struct LogOpUpdateDetails* loud,
        uint64_t flags
        ) 
    {
        BSONObj newObjModified = newObj;
        d->updateObject(pk, oldObj, newObjModified, flags);
        if (loud && loud->logop) {
            OpLogHelpers::logUpdate(
                loud->ns,
                pk,
                oldObj,
                newObjModified,
                loud->fromMigrate,
                &cc().txn()
                );
        }
        if (nsdt != NULL) {
            nsdt->notifyOfWriteOp();
        }
    }

    static void checkNoMods( const BSONObj &o ) {
        BSONObjIterator i( o );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            uassert( 10154 ,  "Modifiers and non-modifiers cannot be mixed", e.fieldName()[ 0 ] != '$' );
        }
    }

    static void checkTooLarge(const BSONObj& newObj) {
        uassert( 12522 , "$ operator made object too large" , newObj.objsize() <= BSONObjMaxUserSize );
    }

    static void updateUsingMods(NamespaceDetails *d, NamespaceDetailsTransient *nsdt,
            const BSONObj &pk, const BSONObj &obj, ModSetState &mss, struct LogOpUpdateDetails* loud) {

        BSONObj newObj = mss.createNewFromMods();
        checkTooLarge( newObj );
        TOKULOG(3) << "updateUsingMods used mod set, transformed " << obj << " to " << newObj << endl;

        updateOneObject( d, nsdt, pk, obj, newObj, loud );
    }

    static void updateNoMods(NamespaceDetails *d, NamespaceDetailsTransient *nsdt,
            const BSONObj &pk, const BSONObj &obj, const BSONObj &updateobj, struct LogOpUpdateDetails* loud) {

        BSONElementManipulator::lookForTimestamps( updateobj );
        checkNoMods( updateobj );
        TOKULOG(3) << "updateNoMods replacing pk " << pk << ", obj " << obj << " with updateobj " << updateobj << endl;

        updateOneObject( d, nsdt, pk, obj, updateobj, loud );
    }

    static void insertAndLog(const char *ns, NamespaceDetails *d, NamespaceDetailsTransient *nsdt,
            BSONObj &newObj, bool logop, bool fromMigrate) {

        checkNoMods( newObj );
        TOKULOG(3) << "insertAndLog for upsert: " << newObj << endl;

        // We cannot pass NamespaceDetails::NO_UNIQUE_CHECKS because we still need to check secondary indexes.
        // We know if we are in this function that we did a query for the object and it didn't exist yet, so the unique check on the PK won't fail.
        // To prove this to yourself, look at the callers of insertAndLog and see that they return an UpdateResult that says the object didn't exist yet.
        insertOneObject(d, nsdt, newObj);
        if (logop) {
            OpLogHelpers::logInsert(ns, newObj, &cc().txn());
        }
    }

    static bool mayUpdateById(NamespaceDetails *d, const BSONObj &patternOrig) {
        if ( isSimpleIdQuery(patternOrig) ) {
            for (int i = 0; i < d->nIndexesBeingBuilt(); i++) {
                IndexDetails &idx = d->idx(i);
                if (idx.info()["clustering"].trueValue()) {
                    return false;
                }
            }
            // We may update by _id, since:
            // - The query is a simple _id query
            // - The modifications do not affect any indexed fields
            // - There are no clustering secondary keys.
            return true;
        }
        return false;
    }

    /* note: this is only (as-is) called for

             - not multi
             - not mods is indexed
             - not upsert
    */
    static UpdateResult _updateById(const BSONObj &pk,
                                    bool isOperatorUpdate,
                                    ModSet* mods,
                                    NamespaceDetails* d,
                                    NamespaceDetailsTransient *nsdt,
                                    const char* ns,
                                    const BSONObj& updateobj,
                                    BSONObj patternOrig,
                                    bool logop,
                                    OpDebug& debug,
                                    bool fromMigrate = false) {

        BSONObj obj;
        {
            TOKULOG(3) << "_updateById looking for pk " << pk << endl;
            dassert(pk == patternOrig["_id"].wrap(""));
            bool found = d->findById( patternOrig, obj );
            TOKULOG(3) << "_updateById findById() got " << obj << endl;
            if ( !found ) {
                // no upsert support in _updateById yet, so we are done.
                return UpdateResult( 0 , 0 , 0 , BSONObj() );
            }
        }

        verify(nsdt);
        nsdt->notifyOfWriteOp();

        /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
           regular ones at the moment. */
        struct LogOpUpdateDetails loud;
        loud.logop = logop;
        loud.ns = ns;
        loud.fromMigrate = fromMigrate;
        if ( isOperatorUpdate ) {
            auto_ptr<ModSetState> mss = mods->prepare( obj );

            // mod set update, ie: $inc: 10 increments by 10.
            updateUsingMods( d, nsdt, pk, obj, *mss, &loud );
            return UpdateResult( 1 , 1 , 1 , BSONObj() );

        } // end $operator update

        // replace-style update
        updateNoMods( d, nsdt, pk, obj, updateobj, &loud );
        return UpdateResult( 1 , 0 , 1 , BSONObj() );
    }

    UpdateResult _updateObjects( const char* ns,
                                 const BSONObj& updateobj,
                                 const BSONObj& patternOrig,
                                 bool upsert,
                                 bool multi,
                                 bool logop ,
                                 OpDebug& debug,
                                 bool fromMigrate,
                                 const QueryPlanSelectionPolicy& planPolicy ) {

        TOKULOG(2) << "update: " << ns
                   << " update: " << updateobj
                   << " query: " << patternOrig
                   << " upsert: " << upsert << " multi: " << multi << endl;

        debug.updateobj = updateobj;

        NamespaceDetails *d = getAndMaybeCreateNS(ns, logop);
        NamespaceDetailsTransient *nsdt = &NamespaceDetailsTransient::get(ns);

        auto_ptr<ModSet> mods;
        const bool isOperatorUpdate = updateobj.firstElementFieldName()[0] == '$';
        bool modsAreIndexed = false;

        if ( isOperatorUpdate ) {
            if ( d->indexBuildInProgress() ) {
                set<string> bgKeys;
                d->inProgIdx().keyPattern().getFieldNames(bgKeys);
                mods.reset( new ModSet(updateobj, nsdt->indexKeys(), &bgKeys) );
            }
            else {
                mods.reset( new ModSet(updateobj, nsdt->indexKeys()) );
            }
            modsAreIndexed = mods->isIndexed();
        }


        int idIdxNo = -1;
        if ( planPolicy.permitOptimalIdPlan() && !multi && !modsAreIndexed &&
             (idIdxNo = d->findIdIndex()) >= 0 && mayUpdateById(d, patternOrig) ) {
            debug.idhack = true;
            IndexDetails &idx = d->idx(idIdxNo);
            BSONObj pk = idx.getKeyFromQuery(patternOrig);
            TOKULOG(3) << "_updateObjects using simple _id query, pattern " << patternOrig << ", pk " << pk << endl;
            UpdateResult result = _updateById( pk,
                                               isOperatorUpdate,
                                               mods.get(),
                                               d,
                                               nsdt,
                                               ns,
                                               updateobj,
                                               patternOrig,
                                               logop,
                                               debug,
                                               fromMigrate);
            if ( result.existing || ! upsert ) {
                return result;
            }
            else if ( upsert && ! isOperatorUpdate && ! logop) {
                // this handles repl inserts
                checkNoMods( updateobj );
                debug.upsert = true;
                BSONObj objModified = updateobj;
                insertOneObject( d, nsdt, objModified );
                return UpdateResult( 0 , 0 , 1 , updateobj );
            }
        }

        int numModded = 0;
        debug.nscanned = 0;
        shared_ptr<Cursor> c =
                NamespaceDetailsTransient::getCursor( ns, patternOrig, BSONObj(), planPolicy );

        if( c->ok() ) {
            set<BSONObj> seenObjects;
            MatchDetails details;
            auto_ptr<ClientCursor> cc;
            do {

                debug.nscanned++;

                if ( mods.get() && mods->hasDynamicArray() ) {
                    // The Cursor must have a Matcher to record an elemMatchKey.  But currently
                    // a modifier on a dynamic array field may be applied even if there is no
                    // elemMatchKey, so a matcher cannot be required.
                    //verify( c->matcher() );
                    details.requestElemMatchKey();
                }

                if ( !c->currentMatches( &details ) ) {
                    c->advance();
                    continue;
                }

                BSONObj currPK = c->currPK();
                if ( c->getsetdup( currPK ) ) {
                    c->advance();
                    continue;
                }

                BSONObj currentObj = c->current();
                BSONObj pattern = patternOrig;

                if ( logop ) {
                    BSONObjBuilder idPattern;
                    BSONElement id;
                    // NOTE: If the matching object lacks an id, we'll log
                    // with the original pattern.  This isn't replay-safe.
                    // It might make sense to suppress the log instead
                    // if there's no id.
                    if ( currentObj.getObjectID( id ) ) {
                        idPattern.append( id );
                        pattern = idPattern.obj();
                    }
                    else {
                        uassert( 10157 ,  "multi-update requires all modified objects to have an _id" , ! multi );
                    }
                }

                /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
                   regular ones at the moment. */
                struct LogOpUpdateDetails loud;
                loud.logop = logop;
                loud.ns = ns;
                loud.fromMigrate = fromMigrate;
                if ( isOperatorUpdate ) {

                    if ( multi ) {
                        // Make our own copies of the currPK and currentObj before we invalidate
                        // them by advancing the cursor.
                        currPK = currPK.copy();
                        currentObj = currentObj.copy();

                        // Advance past the document to be modified. This used to be because of SERVER-5198,
                        // but TokuMX does it because we want to avoid needing to do manual deduplication
                        // of this PK on the next iteration if the current update modifies the next
                        // entry in the index. For example, an index scan over a:1 with mod {$inc: {a:1}}
                        // would cause every other key read to be a duplicate if we didn't advance here.
                        while ( c->ok() && currPK == c->currPK() ) {
                            c->advance();
                        }

                        // Multi updates need to do their own deduplication because updates may modify the
                        // keys the cursor is in the process of scanning over.
                        if ( seenObjects.count( currPK ) ) {
                            continue;
                        } else {
                            seenObjects.insert( currPK );
                        }
                    }

                    ModSet* useMods = mods.get();

                    auto_ptr<ModSet> mymodset;
                    if ( details.hasElemMatchKey() && mods->hasDynamicArray() ) {
                        useMods = mods->fixDynamicArray( details.elemMatchKey() );
                        mymodset.reset( useMods );
                    }

                    auto_ptr<ModSetState> mss = useMods->prepare( currentObj );
                    updateUsingMods( d, nsdt, currPK, currentObj, *mss, &loud );

                    numModded++;
                    if ( ! multi )
                        return UpdateResult( 1 , 1 , numModded , BSONObj() );

                    continue;
                } // end if operator is update

                uassert( 10158 ,  "multi update only works with $ operators" , ! multi );

                updateNoMods( d, nsdt, currPK, currentObj, updateobj, &loud );

                return UpdateResult( 1 , 0 , 1 , BSONObj() );
            } while ( c->ok() );
        } // endif

        if ( numModded )
            return UpdateResult( 1 , 1 , numModded , BSONObj() );

        if ( upsert ) {
            BSONObj newObj = updateobj;
            if ( updateobj.firstElementFieldName()[0] == '$' ) {
                // upsert of an $operation. build a default object
                BSONObj newObj = mods->createNewFromQuery( patternOrig );
                debug.fastmodinsert = true;
                insertAndLog( ns, d, nsdt, newObj, logop, fromMigrate );
                return UpdateResult( 0 , 1 , 1 , newObj );
            }
            uassert( 10159 ,  "multi update only works with $ operators" , ! multi );
            debug.upsert = true;
            insertAndLog( ns, d, nsdt, newObj, logop, fromMigrate );
            return UpdateResult( 0 , 0 , 1 , newObj );
        }

        return UpdateResult( 0 , isOperatorUpdate , 0 , BSONObj() );
    }

    void validateUpdate( const char* ns , const BSONObj& updateobj, const BSONObj& patternOrig ) {
        uassert( 10155 , "cannot update reserved $ collection", strchr(ns, '$') == 0 );
        if ( strstr(ns, ".system.") ) {
            /* dm: it's very important that system.indexes is never updated as IndexDetails
               has pointers into it */
            uassert( 10156,
                     str::stream() << "cannot update system collection: "
                                   << ns << " q: " << patternOrig << " u: " << updateobj,
                     legalClientSystemNS( ns , true ) );
        }
    }

    UpdateResult updateObjects( const char* ns,
                                const BSONObj& updateobj,
                                const BSONObj& patternOrig,
                                bool upsert,
                                bool multi,
                                bool logop ,
                                OpDebug& debug,
                                bool fromMigrate,
                                const QueryPlanSelectionPolicy& planPolicy ) {

        validateUpdate( ns , updateobj , patternOrig );

        UpdateResult ur = _updateObjects(ns, updateobj, patternOrig,
                                         upsert, multi, logop,
                                         debug, fromMigrate, planPolicy );
        debug.nupdated = ur.num;
        return ur;
    }

}  // namespace mongo
