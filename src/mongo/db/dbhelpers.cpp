// dbhelpers.cpp

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

#include <fstream>

#include "mongo/pch.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/json.h"
#include "mongo/db/cursor.h"
#include "mongo/db/oplog.h"
#include "mongo/db/queryoptimizercursor.h"
#include "mongo/db/repl_block.h"
#include "mongo/db/database.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/ops/count.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/oplog_helpers.h"

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>

namespace mongo {

    const BSONObj reverseNaturalObj = BSON( "$natural" << -1 );

    void Helpers::ensureIndex(const char *ns, BSONObj keyPattern, bool unique, const char *name) {
        NamespaceDetails *d = nsdetails(ns);
        if( d == 0 )
            return;

        {
            NamespaceDetails::IndexIterator i = d->ii();
            while( i.more() ) {
                if( i.next().keyPattern().woCompare(keyPattern) == 0 )
                    return;
            }
        }

        if( d->nIndexes() >= NamespaceDetails::NIndexesMax ) {
            problem() << "Helper::ensureIndex fails, MaxIndexes exceeded " << ns << '\n';
            return;
        }

        string system_indexes = cc().database()->name() + ".system.indexes";

        BSONObjBuilder b;
        b.append("name", name);
        b.append("ns", ns);
        b.append("key", keyPattern);
        b.appendBool("unique", unique);
        BSONObj o = b.done();

        insertObject(system_indexes.c_str(), o, 0, true);
    }

    /* fetch a single object from collection ns that matches query
       set your db SavedContext first
    */
    bool Helpers::findOne(const char *ns, const BSONObj &query, BSONObj& result, bool requireIndex) {
        BSONObj obj = findOne( ns, query, requireIndex );
        if ( obj.isEmpty() )
            return false;
        result = obj;
        return true;
    }

    /* fetch a single object from collection ns that matches query
       set your db SavedContext first
    */
    BSONObj Helpers::findOne(const char *ns, const BSONObj &query, bool requireIndex) {
        shared_ptr<Cursor> c =
            NamespaceDetailsTransient::getCursor( ns, query, BSONObj(),
                                                  requireIndex ?
                                                  QueryPlanSelectionPolicy::indexOnly() :
                                                  QueryPlanSelectionPolicy::any() );
        while( c->ok() ) {
            if ( c->currentMatches() && !c->getsetdup( c->currPK() ) ) {
                return c->current().copy();
            }
            c->advance();
        }
        return BSONObj();
    }


    bool Helpers::findById( const char *ns, BSONObj query, BSONObj& result ) {
        Lock::assertAtLeastReadLocked(ns);
        NamespaceDetails *d = nsdetails(ns);
        return d != NULL ? d->findById(query, result) : false;
    }

    shared_ptr<Cursor> Helpers::findTableScan(const char *ns, const BSONObj &order) {
        const int direction = order.getField("$natural").number() >= 0 ? 1 : -1;
        NamespaceDetails *d = nsdetails(ns);
        return shared_ptr<Cursor>( BasicCursor::make(d, direction) );
    }

    vector<BSONObj> Helpers::findAll( const string& ns , const BSONObj& query ) {
        Lock::assertAtLeastReadLocked( ns );

        vector<BSONObj> all;

        Client::Context tx( ns );
        
        shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns.c_str(), query );

        while( c->ok() ) {
            if ( c->currentMatches() && !c->getsetdup( c->currPK() ) ) {
                all.push_back( c->current().copy() );
            }
            c->advance();
        }

        return all;
    }

    bool Helpers::isEmpty(const char *ns, bool doAuth) {
        Client::Context context(ns, dbpath, doAuth);
        shared_ptr<Cursor> c = findTableScan(ns, BSONObj());
        return !c->ok();
    }

    /* Get the first object from a collection.  Generally only useful if the collection
       only ever has a single object -- which is a "singleton collection.

       Returns: true if object exists.
    */
    bool Helpers::getSingleton(const char *ns, BSONObj& result) {
        Client::Context context(ns);

        shared_ptr<Cursor> c = findTableScan(ns, BSONObj());
        if ( !c->ok() ) {
            context.getClient()->curop()->done();
            return false;
        }

        result = c->current().copy();
        context.getClient()->curop()->done();
        return true;
    }

    bool Helpers::getFirst(const char *ns, BSONObj& result) {
        return getSingleton(ns, result);
    }

    bool Helpers::getLast(const char *ns, BSONObj& result) {
        Client::Context ctx(ns);
        shared_ptr<Cursor> c = findTableScan(ns, reverseNaturalObj);
        if( !c->ok() )
            return false;
        result = c->current().copy();
        return true;
    }

    void Helpers::putSingleton(const char *ns, BSONObj obj) {
        OpDebug debug;
        Client::Context context(ns);
        updateObjects(ns, obj, /*pattern=*/BSONObj(), /*upsert=*/true, /*multi=*/false , /*logtheop=*/true , debug );
        context.getClient()->curop()->done();
    }

    void Helpers::putSingletonGod(const char *ns, BSONObj obj, bool logTheOp) {
        OpDebug debug;
        Client::Context context(ns);
        _updateObjects(/*god=*/true, ns, obj, /*pattern=*/BSONObj(), /*upsert=*/true, /*multi=*/false , logTheOp , debug );
        context.getClient()->curop()->done();
    }

    BSONObj Helpers::toKeyFormat( const BSONObj& o , BSONObj& key ) {
        BSONObjBuilder me;
        BSONObjBuilder k;

        BSONObjIterator i( o );
        while ( i.more() ) {
            BSONElement e = i.next();
            k.append( e.fieldName() , 1 );
            me.appendAs( e , "" );
        }
        key = k.obj();
        return me.obj();
    }

    BSONObj Helpers::modifiedRangeBound( const BSONObj& bound ,
                                         const BSONObj& keyPattern ,
                                         int minOrMax ){
        BSONObjBuilder newBound;

        BSONObjIterator src( bound );
        BSONObjIterator pat( keyPattern );

        while( src.more() ){
            massert( 16341 ,
                     str::stream() << "keyPattern " << keyPattern
                                   << " shorter than bound " << bound ,
                     pat.more() );
            BSONElement srcElt = src.next();
            BSONElement patElt = pat.next();
            massert( 16333 ,
                     str::stream() << "field names of bound " << bound
                                   << " do not match those of keyPattern " << keyPattern ,
                     str::equals( srcElt.fieldName() , patElt.fieldName() ) );
            newBound.appendAs( srcElt , "" );
        }
        while( pat.more() ){
            BSONElement patElt = pat.next();
            // for non 1/-1 field values, like {a : "hashed"}, treat order as ascending
            int order = patElt.isNumber() ? patElt.numberInt() : 1;
            if( minOrMax * order == 1 ){
                newBound.appendMaxKey("");
            }
            else {
                newBound.appendMinKey("");
            }
        }
        return newBound.obj();
    }

    long long Helpers::removeRange( const string& ns ,
                                    const BSONObj& min ,
                                    const BSONObj& max ,
                                    const BSONObj& keyPattern ,
                                    bool maxInclusive ,
                                    bool fromMigrate ) {
        long long numDeleted = 0;

        Client::ReadContext ctx(ns);
        Client::Transaction txn(DB_SERIALIZABLE);

        NamespaceDetails *d = nsdetails( ns.c_str() );
        NamespaceDetailsTransient *nsdt = &NamespaceDetailsTransient::get( ns.c_str() );
        IndexDetails &i = d->idx(d->findIndexByKeyPattern(keyPattern));
        // Extend min to get (min, MinKey, MinKey, ....)
        BSONObj newMin = Helpers::modifiedRangeBound( min , keyPattern , -1 );
        // If upper bound is included, extend max to get (max, MaxKey, MaxKey, ...)
        // If not included, extend max to get (max, MinKey, MinKey, ....)
        int minOrMax = maxInclusive ? 1 : -1;
        BSONObj newMax = Helpers::modifiedRangeBound( max , keyPattern , minOrMax );

        for (IndexCursor c(d, i, newMin, newMax, maxInclusive, 1); c.ok(); c.advance()) {
            BSONObj pk = c.currPK();
            BSONObj obj = c.current();
            OpLogHelpers::logDelete(ns.c_str(), obj, fromMigrate, &cc().txn());
            deleteOneObject(d, nsdt, pk, obj);
            numDeleted++;
        }

        txn.commit();
        return numDeleted;
    }

    void Helpers::emptyCollection(const char *ns) {
        Client::Context context(ns);
        deleteObjects(ns, BSONObj(), false);
    }

} // namespace mongo
