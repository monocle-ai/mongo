/**
*    Copyright (C) 2012 Tokutek Inc.
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
#include "mongo/bson/bsonobjbuilder.h"

#include <db.h>

namespace mongo {

    class GTID {
        uint64_t _primarySeqNo;
        uint64_t _GTSeqNo;
        public:
        static int cmp(GTID a, GTID b);
        GTID();
        GTID(uint64_t primarySeqNo, uint64_t GTSeqNo);
        GTID(BSONObj b);
        ~GTID(){};
        BSONObj getBSON();
        void inc();
        void inc_primary();
        
        GTID operator=(const GTID& s);
    };

    class GTIDManager {
        boost::mutex _lock;
        GTID _lastGTID;
        public:            
        GTIDManager( GTID lastGTID );
        ~GTIDManager();
        // returns a GTID that is an increment of _lastGTID
        // also notes that GTID has been handed out
        GTID getGTID();
        // notification that user of GTID has completed work
        // and either committed or aborted transaction associated with
        // GTID
        void noteGTIDDone();
    };
} // namespace mongo
