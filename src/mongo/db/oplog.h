// oplog.h - writing to and reading from oplog

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

/*

     local.oplog.$main is the default
*/

#pragma once

#include "db.h"
#include "dbhelpers.h"
#include "clientcursor.h"
#include "../util/optime.h"
#include "../util/timer.h"

namespace mongo {

    void createOplog();
    void logToReplInfo(GTID minLiveGTID, GTID minUnappliedGTID);

    void logOp( const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt = 0, bool fromMigrate = false );
    // Write operations to the log (local.oplog.$main)
    void logTransactionOps(GTID gtid, BSONArray& opInfo);
    void deleteOplogFiles();
    void openOplogFiles();
    
    GTID getGTIDFromOplogEntry(BSONObj o);
    bool getLastGTIDinOplog(GTID* gtid);
    bool gtidExistsInOplog(GTID gtid);
    void writeEntryToOplog(BSONObj entry);
    
    /** puts obj in the oplog as a comment (a no-op).  Just for diags.
        convention is
          { msg : "text", ... }
    */
    void oplogCheckCloseDatabase( Database * db );

    extern int __findingStartInitialTimeout; // configurable for testing

    class QueryPlan;
    
    class Sync {
    protected:
        string hn;
    public:
        Sync(const string& hostname) : hn(hostname) {}
        virtual ~Sync() {}
        virtual BSONObj getMissingDoc(const BSONObj& o);

        /**
         * If applyOperation_inlock should be called again after an update fails.
         */
        virtual bool shouldRetry(const BSONObj& o);
        void setHostname(const string& hostname);
    };

    /**
     * take an op and apply locally
     * used for applying from an oplog
     * @param fromRepl really from replication or for testing/internal/command/etc...
     * Returns if the op was an update that could not be applied (true on failure)
     */
    bool applyOperation_inlock(const BSONObj& op, bool fromRepl = true, bool convertUpdateToUpsert = false);
}
