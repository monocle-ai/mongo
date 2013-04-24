// count.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "count.h"

#include "../client.h"
#include "../clientcursor.h"
#include "../namespace.h"
#include "../queryutil.h"
#include "mongo/client/dbclientinterface.h"

namespace mongo {

    long long runCount( const char *ns, const BSONObj &cmd, string &err, int &errCode ) {
        Client::Context ctx(ns);
        NamespaceDetails *d = nsdetails( ns );
        if ( !d ) {
            err = "ns missing";
            return -1;
        }
        BSONObj query = cmd.getObjectField("query");

        // count of all objects
        if ( query.isEmpty() ) {
            // TODO: TokuDB: call this with in-memory stats once we maintain them
            //return applySkipLimit( d->stats.nrecords , cmd );
        }

        long long count = 0;
        long long skip = cmd["skip"].numberLong();
        long long limit = cmd["limit"].numberLong();

        if ( limit < 0 ) {
            limit  = -limit;
        }

        TokuCommandSettings settings;
        settings.setBulkFetch(true);
        settings.setQueryCursorMode(DEFAULT_LOCK_CURSOR);
        cc().setTokuCommandSettings(settings);

        Lock::assertAtLeastReadLocked(ns);
        Client::Transaction transaction(DB_TXN_SNAPSHOT | DB_TXN_READ_ONLY);
        try {
            bool simpleEqualityMatch = false;
            {
                shared_ptr<Cursor> cursor =
                        NamespaceDetailsTransient::getCursor( ns, query, BSONObj(), QueryPlanSelectionPolicy::any(),
                                                              &simpleEqualityMatch );
                for ( ; cursor->ok() ; cursor->advance() ) {
                    // With simple equality matching there is no need to use the matcher because the bounds
                    // are enforced by the FieldRangeVectorIterator and only key fields have constraints.  There
                    // is no need to do key deduping because an exact value is specified in the query for all key
                    // fields and duplicate keys are not allowed per document.
                    // NOTE In the distant past we used a min/max bounded IndexCursor with a shallow
                    // equality comparison to check for matches in the simple match case.  That may be
                    // more performant, but I don't think we've measured the performance.
                    if ( simpleEqualityMatch ||
                         ( cursor->currentMatches() && !cursor->getsetdup( cursor->currPK() ) ) ) {
                        if ( skip > 0 ) {
                            --skip;
                        }
                        else {
                            ++count;
                            if ( limit > 0 && count >= limit ) {
                                break;
                            }
                        }
                    }
                }
            }
            transaction.commit();
            return count;
        }
        catch ( const DBException &e ) {
            err = e.toString();
            errCode = e.getCode();
            count = -2;
        }
        catch ( const std::exception &e ) {
            err = e.what();
            errCode = 0;
            count = -2;
        }
        if ( count != -2 ) { // keeping the magical -2 return value for legacy reasons...
            transaction.commit();
        } else {
            // Historically we have returned zero in many count assertion cases - see SERVER-2291.
            log() << "Count with ns: " << ns << " and query: " << query
                  << " failed with exception: " << err << " code: " << errCode
                  << endl;
        }
        return count;
    }

} // namespace mongo
