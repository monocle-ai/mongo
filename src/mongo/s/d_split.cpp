// @file  d_split.cpp

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

#include <map>
#include <string>

#include "mongo/pch.h"

#include "mongo/db/cursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/instance.h"
#include "mongo/db/queryoptimizer.h"
#include "mongo/db/clientcursor.h"
#include "mongo/client/dbclientcursor.h"

#include "mongo/client/connpool.h"
#include "mongo/client/distlock.h"
#include "mongo/util/timer.h"

#include "mongo/s/chunk.h" // for static genID only
#include "mongo/s/config.h"
#include "mongo/s/d_logic.h"

namespace mongo {


    class CmdMedianKey : public Command {
    public:
        CmdMedianKey() : Command( "medianKey" ) {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream &help ) const {
            help << "Deprecated internal command. Use splitVector command instead. \n";
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            errmsg = "medianKey command no longer supported. Calling this indicates mismatch between mongo versions.";
            return false;
        }
    } cmdMedianKey;

    class CheckShardingIndex : public Command {
    public:
        CheckShardingIndex() : Command( "checkShardingIndex" , false ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream &help ) const {
            help << "Internal command.\n";
        }

        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            const char* ns = jsobj.getStringField( "checkShardingIndex" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );

            if ( keyPattern.isEmpty() ) {
                errmsg = "no key pattern found in checkShardingindex";
                return false;
            }

            if ( keyPattern.nFields() == 1 && str::equals( "_id" , keyPattern.firstElementFieldName() ) ) {
                result.appendBool( "idskip" , true );
                return true;
            }

            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            if ( min.isEmpty() != max.isEmpty() ) {
                errmsg = "either provide both min and max or leave both empty";
                return false;
            }

            Client::ReadContext ctx( ns );
            ctx.ctx().beginTransaction();
            NamespaceDetails *d = nsdetails( ns );
            if ( ! d ) {
                errmsg = "ns not found";
                return false;
            }

            const IndexDetails *idx = d->findIndexByPrefix( keyPattern ,
                                                            true );  /* require single key */
            if ( idx == NULL ) {
                errmsg = "couldn't find valid index for shard key";
                return false;
            }
            // extend min to get (min, MinKey, MinKey, ....)
            min = Helpers::modifiedRangeBound( min , idx->keyPattern() , -1 );
            if  ( max.isEmpty() ) {
                // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
                max = Helpers::modifiedRangeBound( max , idx->keyPattern() , 1 );
            } else {
                // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
                max = Helpers::modifiedRangeBound( max , idx->keyPattern() , -1 );
            }

            IndexCursor * idxCursor = new IndexCursor( d , idx , min , max , false , 1 );
            shared_ptr<Cursor> c( idxCursor );
            auto_ptr<ClientCursor> cc( new ClientCursor( QueryOption_NoCursorTimeout , c , ns ) );
            if ( ! cc->ok() ) {
                // range is empty
                ctx.ctx().commitTransaction();
                return true;
            }

            // for now, the only check is that all shard keys are filled
            // null is ok, 
            // TODO if $exist for nulls were picking the index, it could be used instead efficiently
            int keyPatternLength = keyPattern.nFields();
            while ( cc->ok() ) {
                BSONObj currKey = c->currKey();
                
                //check that current key contains non-null elements for all fields in keyPattern
                BSONObjIterator i( currKey );
                for( int k = 0; k < keyPatternLength ; k++ ) {
                    if( ! i.more() ) {
                        errmsg = str::stream() << "index key " << currKey
                                               << " too short for pattern " << keyPattern;
                        return false;
                    }
                    BSONElement currKeyElt = i.next();

                    if ( currKeyElt.type() && currKeyElt.type() != jstNULL )
                        continue;

                    BSONObj obj = c->current();
                    BSONObjIterator j( keyPattern );
                    BSONElement real;
                    for ( int x=0; x <= k; x++ )
                        real = j.next();

                    real = obj.getFieldDotted( real.fieldName() );

                    if ( real.type() )
                        continue;
                    
                    ostringstream os;
                    os << "found null value in key " << idxCursor->prettyKey( currKey ) << " for doc: "
                       << ( obj.hasField( "_id" ) ? obj.toString() : obj["_id"].toString() );
                    log() << "checkShardingIndex for '" << ns << "' failed: " << os.str() << endl;
                    
                    errmsg = os.str();
                    return false;
                }
                cc->advance();

#if 0
                if ( ! cc->yieldSometimes( ClientCursor::DontNeed ) ) {
                    cc.release();
                    break;
                }
#endif
            }

            ctx.ctx().commitTransaction();
            return true;
        }
    } cmdCheckShardingIndex;

    class SplitVector : public Command {
    public:
        SplitVector() : Command( "splitVector" , false ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream &help ) const {
            help <<
                 "Internal command.\n"
                 "examples:\n"
                 "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, maxChunkSize:200 }\n"
                 "  maxChunkSize unit in MBs\n"
                 "  May optionally specify 'maxSplitPoints' and 'maxChunkObjects' to avoid traversing the whole chunk\n"
                 "  \n"
                 "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, force: true }\n"
                 "  'force' will produce one split point even if data is small; defaults to false\n"
                 "NOTE: This command may take a while to run";
        }

        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            //
            // 1.a We'll parse the parameters in two steps. First, make sure the we can use the split index to get
            //     a good approximation of the size of the chunk -- without needing to access the actual data.
            //

            const char* ns = jsobj.getStringField( "splitVector" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );

            if ( keyPattern.isEmpty() ) {
                errmsg = "no key pattern found in splitVector";
                return false;
            }

            // If min and max are not provided use the "minKey" and "maxKey" for the sharding key pattern.
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            if ( min.isEmpty() != max.isEmpty() ){
                errmsg = "either provide both min and max or leave both empty";
                return false;
            }

            long long maxSplitPoints = 0;
            BSONElement maxSplitPointsElem = jsobj[ "maxSplitPoints" ];
            if ( maxSplitPointsElem.isNumber() ) {
                maxSplitPoints = maxSplitPointsElem.numberLong();
            }

            long long maxChunkObjects = Chunk::MaxObjectPerChunk;
            BSONElement MaxChunkObjectsElem = jsobj[ "maxChunkObjects" ];
            if ( MaxChunkObjectsElem.isNumber() ) {
                maxChunkObjects = MaxChunkObjectsElem.numberLong();
            }

            vector<BSONObj> splitKeys;

            {
                // Get the size estimate for this namespace
                Client::ReadContext ctx( ns );
                NamespaceDetails *d = nsdetails( ns );
                if ( ! d ) {
                    errmsg = "ns not found";
                    return false;
                }
                
                const IndexDetails *idx = d->findIndexByPrefix( keyPattern ,
                                                                true ); /* require single key */
                if ( idx == NULL ) {
                    errmsg = (string)"couldn't find index over splitting key " +
                             keyPattern.clientReadable().toString();
                    return false;
                }
                // extend min to get (min, MinKey, MinKey, ....)
                min = Helpers::modifiedRangeBound( min , idx->keyPattern() , -1 );
                if  ( max.isEmpty() ) {
                    // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
                    max = Helpers::modifiedRangeBound( max , idx->keyPattern() , 1 );
                } else {
                    // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
                    max = Helpers::modifiedRangeBound( max , idx->keyPattern() , -1 );
                }
                
                const long long recCount = 10; ::abort(); //d->stats.nrecords;
                const long long dataSize = 10; ::abort(); // d->stats.datasize;
                
                //
                // 1.b Now that we have the size estimate, go over the remaining parameters and apply any maximum size
                //     restrictions specified there.
                //
                
                // 'force'-ing a split is equivalent to having maxChunkSize be the size of the current chunk, i.e., the
                // logic below will split that chunk in half
                long long maxChunkSize = 0;
                bool force = false;
                {
                    BSONElement maxSizeElem = jsobj[ "maxChunkSize" ];
                    BSONElement forceElem = jsobj[ "force" ];
                    
                    if ( forceElem.trueValue() ) {
                        force = true;
                        maxChunkSize = dataSize;
                        
                    }
                    else if ( maxSizeElem.isNumber() ) {
                        maxChunkSize = maxSizeElem.numberLong() * 1<<20;
                        
                    }
                    else {
                        maxSizeElem = jsobj["maxChunkSizeBytes"];
                        if ( maxSizeElem.isNumber() ) {
                            maxChunkSize = maxSizeElem.numberLong();
                        }
                    }
                    
                    if ( maxChunkSize <= 0 ) {
                        errmsg = "need to specify the desired max chunk size (maxChunkSize or maxChunkSizeBytes)";
                        return false;
                    }
                }
                
                
                // If there's not enough data for more than one chunk, no point continuing.
                if ( dataSize < maxChunkSize || recCount == 0 ) {
                    vector<BSONObj> emptyVector;
                    result.append( "splitKeys" , emptyVector );
                    return true;
                }
                
                log() << "request split points lookup for chunk " << ns << " " << min << " -->> " << max << endl;
                
                // We'll use the average object size and number of object to find approximately how many keys
                // each chunk should have. We'll split at half the maxChunkSize or maxChunkObjects, if
                // provided.
                const long long avgRecSize = dataSize / recCount;
                long long keyCount = maxChunkSize / (2 * avgRecSize);
                if ( maxChunkObjects && ( maxChunkObjects < keyCount ) ) {
                    log() << "limiting split vector to " << maxChunkObjects << " (from " << keyCount << ") objects " << endl;
                    keyCount = maxChunkObjects;
                }
                
                //
                // 2. Traverse the index and add the keyCount-th key to the result vector. If that key
                //    appeared in the vector before, we omit it. The invariant here is that all the
                //    instances of a given key value live in the same chunk.
                //
                
                Timer timer;
                long long currCount = 0;
                long long numChunks = 0;
                
                IndexCursor * idxCursor = new IndexCursor( d , idx , min , max , false , 1 );
                shared_ptr<Cursor> c( idxCursor );
                auto_ptr<ClientCursor> cc( new ClientCursor( QueryOption_NoCursorTimeout , c , ns ) );
                if ( ! cc->ok() ) {
                    errmsg = "can't open a cursor for splitting (desired range is possibly empty)";
                    return false;
                }
                
                // Use every 'keyCount'-th key as a split point. We add the initial key as a sentinel, to be removed
                // at the end. If a key appears more times than entries allowed on a chunk, we issue a warning and
                // split on the following key.
                set<BSONObj> tooFrequentKeys;
                splitKeys.push_back( idxCursor->prettyKey( c->currKey().getOwned() ).extractFields( keyPattern ) );
                while ( 1 ) {
                    while ( cc->ok() ) {
                        currCount++;
                        
                        if ( currCount > keyCount ) {
                            BSONObj currKey = idxCursor->prettyKey( c->currKey() ).extractFields(keyPattern);
                            // Do not use this split key if it is the same used in the previous split point.
                            if ( currKey.woCompare( splitKeys.back() ) == 0 ) {
                                tooFrequentKeys.insert( currKey.getOwned() );
                                
                            }
                            else {
                                splitKeys.push_back( currKey.getOwned() );
                                currCount = 0;
                                numChunks++;
                                
                                LOG(4) << "picked a split key: " << currKey << endl;
                            }
                            
                        }
                        
                        cc->advance();
                        
                        // Stop if we have enough split points.
                        if ( maxSplitPoints && ( numChunks >= maxSplitPoints ) ) {
                            log() << "max number of requested split points reached (" << numChunks
                                  << ") before the end of chunk " << ns << " " << min << " -->> " << max
                                  << endl;
                            break;
                        }
                        
#if 0
                        if ( ! cc->yieldSometimes( ClientCursor::DontNeed ) ) {
                            // we were near and and got pushed to the end
                            // i think returning the splits we've already found is fine
                            
                            // don't use the btree cursor pointer to access keys beyond this point but ok
                            // to use it for format the keys we've got already
                            cc.release();
                            break;
                        }
#endif
                    }
                    
                    if ( splitKeys.size() > 1 || ! force )
                        break;
                    
                    force = false;
                    keyCount = currCount / 2;
                    currCount = 0;
                    log() << "splitVector doing another cycle because of force, keyCount now: " << keyCount << endl;
                    
                    idxCursor = new IndexCursor( d , idx , min , max , false , 1 );
                    c.reset( idxCursor );
                    cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , c , ns ) );
                }
                
                //
                // 3. Format the result and issue any warnings about the data we gathered while traversing the
                //    index
                //
                
                // Warn for keys that are more numerous than maxChunkSize allows.
                for ( set<BSONObj>::const_iterator it = tooFrequentKeys.begin(); it != tooFrequentKeys.end(); ++it ) {
                    warning() << "chunk is larger than " << maxChunkSize
                              << " bytes because of key " << idxCursor->prettyKey( *it ) << endl;
                }
                
                // Remove the sentinel at the beginning before returning
                splitKeys.erase( splitKeys.begin() );
                verify( c.get() );
                
                if ( timer.millis() > cmdLine.slowMS ) {
                    warning() << "Finding the split vector for " <<  ns << " over "<< keyPattern
                              << " keyCount: " << keyCount << " numSplits: " << splitKeys.size() 
                              << " lookedAt: " << currCount << " took " << timer.millis() << "ms"
                              << endl;
                }
                
                // Warning: we are sending back an array of keys but are currently limited to
                // 4MB work of 'result' size. This should be okay for now.
                
            }

            result.append( "splitKeys" , splitKeys );

            return true;

        }
    } cmdSplitVector;

    // ** temporary ** 2010-10-22
    // chunkInfo is a helper to collect and log information about the chunks generated in splitChunk.
    // It should hold the chunk state for this module only, while we don't have min/max key info per chunk on the
    // mongod side. Do not build on this; it will go away.
    struct ChunkInfo {
        BSONObj min;
        BSONObj max;
        ShardChunkVersion lastmod;

        ChunkInfo() { }
        ChunkInfo( BSONObj aMin , BSONObj aMax , ShardChunkVersion aVersion ) : min(aMin) , max(aMax) , lastmod(aVersion) {}
        void appendShortVersion( const char* name, BSONObjBuilder& b ) const;
        string toString() const;
    };

    void ChunkInfo::appendShortVersion( const char * name , BSONObjBuilder& b ) const {
        BSONObjBuilder bb( b.subobjStart( name ) );
        bb.append( "min" , min );
        bb.append( "max" , max );
        lastmod.addToBSON( bb, "lastmod" );
        bb.done();
    }

    string ChunkInfo::toString() const {
        ostringstream os;
        os << "lastmod: " << lastmod.toString() << " min: " << min << " max: " << endl;
        return os.str();
    }
    // ** end temporary **

    class SplitChunkCommand : public Command {
    public:
        SplitChunkCommand() : Command( "splitChunk" ) {}
        virtual void help( stringstream& help ) const {
            help <<
                 "internal command usage only\n"
                 "example:\n"
                 " { splitChunk:\"db.foo\" , keyPattern: {a:1} , min : {a:100} , max: {a:200} { splitKeys : [ {a:150} , ... ]}";
        }

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            //
            // 1. check whether parameters passed to splitChunk are sound
            //

            const string ns = cmdObj.firstElement().str();
            if ( ns.empty() ) {
                errmsg  = "need to specify namespace in command";
                return false;
            }

            const BSONObj keyPattern = cmdObj["keyPattern"].Obj();
            if ( keyPattern.isEmpty() ) {
                errmsg = "need to specify the key pattern the collection is sharded over";
                return false;
            }

            const BSONObj min = cmdObj["min"].Obj();
            if ( min.isEmpty() ) {
                errmsg = "need to specify the min key for the chunk";
                return false;
            }

            const BSONObj max = cmdObj["max"].Obj();
            if ( max.isEmpty() ) {
                errmsg = "need to specify the max key for the chunk";
                return false;
            }

            const string from = cmdObj["from"].str();
            if ( from.empty() ) {
                errmsg = "need specify server to split chunk at";
                return false;
            }

            const BSONObj splitKeysElem = cmdObj["splitKeys"].Obj();
            if ( splitKeysElem.isEmpty() ) {
                errmsg = "need to provide the split points to chunk over";
                return false;
            }
            vector<BSONObj> splitKeys;
            BSONObjIterator it( splitKeysElem );
            while ( it.more() ) {
                splitKeys.push_back( it.next().Obj().getOwned() );
            }

            const BSONElement shardId = cmdObj["shardId"];
            if ( shardId.eoo() ) {
                errmsg = "need to provide shardId";
                return false;
            }

            // It is possible that this is the first sharded command this mongod is asked to perform. If so,
            // start sharding apparatus. We'd still be missing some more shard-related info but we'll get it
            // in step 2. below.
            if ( ! shardingState.enabled() ) {
                if ( cmdObj["configdb"].type() != String ) {
                    errmsg = "sharding not enabled";
                    return false;
                }
                string configdb = cmdObj["configdb"].String();
                shardingState.enable( configdb );
                configServer.init( configdb );
            }

            Shard myShard( from );

            log() << "received splitChunk request: " << cmdObj << endl;

            //
            // 2. lock the collection's metadata and get highest version for the current shard
            //

            DistributedLock lockSetup( ConnectionString( shardingState.getConfigServer() , ConnectionString::SYNC) , ns );
            dist_lock_try dlk;

            try{
            	dlk = dist_lock_try( &lockSetup, string("split-") + min.toString() );
            }
            catch( LockException& e ){
            	errmsg = str::stream() << "Error locking distributed lock for split." << causedBy( e );
            	return false;
            }

            if ( ! dlk.got() ) {
                errmsg = "the collection's metadata lock is taken";
                result.append( "who" , dlk.other() );
                return false;
            }

            // TODO This is a check migrate does to the letter. Factor it out and share. 2010-10-22

            ShardChunkVersion maxVersion;
            string shard;
            ChunkInfo origChunk;
            {
                scoped_ptr<ScopedDbConnection> conn(
                        ScopedDbConnection::getInternalScopedDbConnection(
                                shardingState.getConfigServer() ) );

                BSONObj x = conn->get()->findOne( ShardNS::chunk,
                                                  Query( BSON( "ns" << ns ) )
                                                      .sort( BSON( "lastmod" << -1 ) ) );
                maxVersion = ShardChunkVersion::fromBSON( x, "lastmod" );

                BSONObj currChunk = conn->get()->findOne( ShardNS::chunk , shardId.wrap( "_id" ) )
                    .getOwned();
                verify( currChunk["shard"].type() );
                verify( currChunk["min"].type() );
                verify( currChunk["max"].type() );
                shard = currChunk["shard"].String();
                conn->done();

                BSONObj currMin = currChunk["min"].Obj();
                BSONObj currMax = currChunk["max"].Obj();
                if ( currMin.woCompare( min ) || currMax.woCompare( max ) ) {
                    errmsg = "chunk boundaries are outdated (likely a split occurred)";
                    result.append( "currMin" , currMin );
                    result.append( "currMax" , currMax );
                    result.append( "requestedMin" , min );
                    result.append( "requestedMax" , max );

                    log( LL_WARNING ) << "aborted split because " << errmsg << ": " << min << "->" << max
                                      << " is now " << currMin << "->" << currMax << endl;
                    return false;
                }

                if ( shard != myShard.getName() ) {
                    errmsg = "location is outdated (likely balance or migrate occurred)";
                    result.append( "from" , myShard.getName() );
                    result.append( "official" , shard );

                    log( LL_WARNING ) << "aborted split because " << errmsg << ": chunk is at " << shard
                                      << " and not at " << myShard.getName() << endl;
                    return false;
                }

                if ( maxVersion < shardingState.getVersion( ns ) ) {
                    errmsg = "official version less than mine?";
                    maxVersion.addToBSON( result, "officialVersion" );
                    shardingState.getVersion( ns ).addToBSON( result, "myVersion" );

                    log( LL_WARNING ) << "aborted split because " << errmsg << ": official " << maxVersion
                                      << " mine: " << shardingState.getVersion(ns) << endl;
                    return false;
                }

                origChunk.min = currMin.getOwned();
                origChunk.max = currMax.getOwned();
                origChunk.lastmod = ShardChunkVersion::fromBSON( currChunk["lastmod"] );

                // since this could be the first call that enable sharding we also make sure to have the chunk manager up to date
                shardingState.gotShardName( shard );
                ShardChunkVersion shardVersion;
                shardingState.trySetVersion( ns , shardVersion /* will return updated */ );

                log() << "splitChunk accepted at version " << shardVersion << endl;

            }

            //
            // 3. create the batch of updates to metadata ( the new chunks ) to be applied via 'applyOps' command
            //

            BSONObjBuilder logDetail;
            origChunk.appendShortVersion( "before" , logDetail );
            LOG(1) << "before split on " << origChunk << endl;
            vector<ChunkInfo> newChunks;

            ShardChunkVersion myVersion = maxVersion;
            BSONObj startKey = min;
            splitKeys.push_back( max ); // makes it easier to have 'max' in the next loop. remove later.

            BSONObjBuilder cmdBuilder;
            BSONArrayBuilder updates( cmdBuilder.subarrayStart( "applyOps" ) );

            for ( vector<BSONObj>::const_iterator it = splitKeys.begin(); it != splitKeys.end(); ++it ) {
                BSONObj endKey = *it;

                // splits only update the 'minor' portion of version
                myVersion.incMinor();

                // build an update operation against the chunks collection of the config database with
                // upsert true
                BSONObjBuilder op;
                op.append( "op" , "u" );
                op.appendBool( "b" , true );
                op.append( "ns" , ShardNS::chunk );

                // add the modified (new) chunk information as the update object
                BSONObjBuilder n( op.subobjStart( "o" ) );
                n.append( "_id" , Chunk::genID( ns , startKey ) );
                myVersion.addToBSON( n, "lastmod" );
                n.append( "ns" , ns );
                n.append( "min" , startKey );
                n.append( "max" , endKey );
                n.append( "shard" , shard );
                n.done();

                // add the chunk's _id as the query part of the update statement
                BSONObjBuilder q( op.subobjStart( "o2" ) );
                q.append( "_id" , Chunk::genID( ns , startKey ) );
                q.done();

                updates.append( op.obj() );

                // remember this chunk info for logging later
                newChunks.push_back( ChunkInfo( startKey , endKey, myVersion ) );

                startKey = endKey;
            }

            updates.done();

            {
                BSONArrayBuilder preCond( cmdBuilder.subarrayStart( "preCondition" ) );
                BSONObjBuilder b;
                b.append( "ns" , ShardNS::chunk );
                b.append( "q" , BSON( "query" << BSON( "ns" << ns ) << "orderby" << BSON( "lastmod" << -1 ) ) );
                {
                    BSONObjBuilder bb( b.subobjStart( "res" ) );
                    // TODO: For backwards compatibility, we can't yet require an epoch here
                    bb.appendTimestamp( "lastmod", maxVersion.toLong() );
                    bb.done();
                }
                preCond.append( b.obj() );
                preCond.done();
            }

            //
            // 4. apply the batch of updates to metadata and to the chunk manager
            //

            BSONObj cmd = cmdBuilder.obj();

            LOG(1) << "splitChunk update: " << cmd << endl;

            bool ok;
            BSONObj cmdResult;
            {
                scoped_ptr<ScopedDbConnection> conn(
                        ScopedDbConnection::getInternalScopedDbConnection(
                                shardingState.getConfigServer() ) );
                ok = conn->get()->runCommand( "config" , cmd , cmdResult );
                conn->done();
            }

            if ( ! ok ) {
                stringstream ss;
                ss << "saving chunks failed.  cmd: " << cmd << " result: " << cmdResult;
                error() << ss.str() << endl;
                msgasserted( 13593 , ss.str() );
            }

            // install a chunk manager with knowledge about newly split chunks in this shard's state
            splitKeys.pop_back(); // 'max' was used as sentinel
            maxVersion.incMinor();
            shardingState.splitChunk( ns , min , max , splitKeys , maxVersion );

            //
            // 5. logChanges
            //

            // single splits are logged different than multisplits
            if ( newChunks.size() == 2 ) {
                newChunks[0].appendShortVersion( "left" , logDetail );
                newChunks[1].appendShortVersion( "right" , logDetail );
                configServer.logChange( "split" , ns , logDetail.obj() );

            }
            else {
                BSONObj beforeDetailObj = logDetail.obj();
                BSONObj firstDetailObj = beforeDetailObj.getOwned();
                const int newChunksSize = newChunks.size();

                for ( int i=0; i < newChunksSize; i++ ) {
                    BSONObjBuilder chunkDetail;
                    chunkDetail.appendElements( beforeDetailObj );
                    chunkDetail.append( "number", i+1 );
                    chunkDetail.append( "of" , newChunksSize );
                    newChunks[i].appendShortVersion( "chunk" , chunkDetail );
                    configServer.logChange( "multi-split" , ns , chunkDetail.obj() );
                }
            }

            if (newChunks.size() == 2){
                // If one of the chunks has only one object in it we should move it
                static const BSONObj fields = BSON("_id" << 1 );
                DBDirectClient conn;
                for (int i=1; i >= 0 ; i--){ // high chunk more likely to have only one obj
                    ChunkInfo chunk = newChunks[i];
                    Query q = Query().minKey(chunk.min).maxKey(chunk.max);
                    scoped_ptr<DBClientCursor> c (conn.query(ns, q, /*limit*/-2, 0, &fields));
                    if (c && c->itcount() == 1) {
                        result.append("shouldMigrate", BSON("min" << chunk.min << "max" << chunk.max));
                        break;
                    }
                }
            }

            return true;
        }
    } cmdSplitChunk;

}  // namespace mongo
