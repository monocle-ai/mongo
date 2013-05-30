/* @file dbhelpers.h

   db helpers are helper functions and classes that let us easily manipulate the local
   database instance in-proc.
*/

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

#pragma once

#include "mongo/pch.h"
#include "client.h"
#include "db.h"

namespace mongo {

    extern const BSONObj reverseNaturalObj; // {"$natural": -1 }

    class Cursor;
    class CoveredIndexMatcher;

    /**
       all helpers assume locking is handled above them
     */
    namespace Helpers {

        /* ensure the specified index exists.

           @param keyPattern key pattern, e.g., { ts : 1 }
           @param name index name, e.g., "name_1"

           This method can be a little (not much) cpu-slow, so you may wish to use
             OCCASIONALLY ensureIndex(...);

           Note: use ensureHaveIdIndex() for the _id index: it is faster.
           Note: does nothing if collection does not yet exist.
        */
        void ensureIndex(const char *ns, BSONObj keyPattern, bool unique, const char *name);

        /* fetch a single object from collection ns that matches query.
           set your db SavedContext first.

           @param query - the query to perform.  note this is the low level portion of query so "orderby : ..."
                          won't work.

           @param requireIndex if true, assert if no index for the query.  a way to guard against
           writing a slow query.

           @return true if object found
        */
        bool findOne(const char *ns, const BSONObj &query, BSONObj& result, bool requireIndex = false);
        BSONObj findOne(const char *ns, const BSONObj &query, bool requireIndex);

        // Replacement for DataFileMgr::findTableScan
        shared_ptr<Cursor> findTableScan(const char *ns, const BSONObj &order);

        /**
         * have to be locked already
         */
        vector<BSONObj> findAll( const string& ns , const BSONObj& query );

        /**
         * @param foundIndex if passed in will be set to 1 if ns and index found
         * @return true if object found
         */
        bool findById( const char *ns, BSONObj query, BSONObj& result ); 

        /** Get/put the first (or last) object from a collection.  Generally only useful if the collection
            only ever has a single object -- which is a "singleton collection".

            You do not need to set the database (Context) before calling.

            @return true if object exists.
        */
        bool getSingleton(const char *ns, BSONObj& result);
        void putSingleton(const char *ns, BSONObj obj);
        void putSingletonGod(const char *ns, BSONObj obj, bool logTheOp);
        bool getFirst(const char *ns, BSONObj& result);
        bool getLast(const char *ns, BSONObj& result); // get last object int he collection; e.g. {$natural : -1}

        /** You do not need to set the database before calling.
            @return true if collection is empty.
        */
        bool isEmpty(const char *ns, bool doAuth=true);

        // TODO: this should be somewhere else probably
        /* Takes object o, and returns a new object with the
         * same field elements but the names stripped out.  Also,
         * fills in "key" with an ascending keyPattern that matches o
         * Example:
         *    o = {a : 5 , b : 6} -->
         *      sets key= {a : 1, b :1}, returns {"" : 5, "" : 6}
         */
        BSONObj toKeyFormat( const BSONObj& o , BSONObj& key );

        /* Takes a BSONObj indicating the min or max boundary of a range,
         * and a keyPattern corresponding to an index that is useful
         * for locating items in the range, and returns an "extension"
         * of the bound, modified to fit the given pattern.  In other words,
         * it appends MinKey or MaxKey values to the bound, so that the extension
         * has the same number of fields as keyPattern.
         * minOrMax should be -1/+1 to indicate whether the extension
         * corresponds to the min or max bound for the range.
         * Also, strips out the field names to put the bound in key format.
         * Examples:
         *   {a : 55}, {a :1}, -1 --> {"" : 55}
         *   {a : 55}, {a : 1, b : 1}, -1 -> {"" : 55, "" : minKey}
         *   {a : 55}, {a : 1, b : 1}, 1 -> {"" : 55, "" : maxKey}
         *   {a : 55}, {a : 1, b : -1}, -1 -> {"" : 55, "" : maxKey}
         *   {a : 55}, {a : 1, b : -1}, 1 -> {"" : 55, "" : minKey}
         *
         * This function is useful for modifying chunk ranges in sharding,
         * when the shard key is a prefix of the index actually used
         * (also useful when the shard key is equal to the index used,
         * since it strips out the field names).
         */
        BSONObj modifiedRangeBound( const BSONObj& bound ,
                                           const BSONObj& keyPattern ,
                                           int minOrMax );

        /**
         * Takes a range, specified by a min and max, and an index, specified by
         * keyPattern, and removes all the documents in that range found by iterating
         * over the given index. Caller is responsible for insuring that min/max are
         * compatible with the given keyPattern (e.g min={a:100} is compatible with
         * keyPattern={a:1,b:1} since it can be extended to {a:100,b:minKey}, but
         * min={b:100} is not compatible).
         *
         * Caller must hold a write lock on 'ns'
         *
         * Does oplog the individual document deletions.
         */
        long long removeRange( const string& ns , 
                                      const BSONObj& min , 
                                      const BSONObj& max , 
                                      const BSONObj& keyPattern ,
                                      bool maxInclusive = false , 
                                      /* RemoveCallback * callback = 0, */
                                      bool fromMigrate = false );

        /**
         * Remove all documents from a collection.
         * You do not need to set the database before calling.
         * Does not oplog the operation.
         */
        void emptyCollection(const char *ns);

    };

} // namespace mongo
