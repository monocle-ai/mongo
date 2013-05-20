// query.cpp

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

#include "pch.h"
#include "query.h"
#include "../clientcursor.h"
#include "../oplog.h"
#include "../../bson/util/builder.h"
#include "../replutil.h"
#include "../scanandorder.h"
#include "../commands.h"
#include "../queryoptimizer.h"
#include "../../s/d_logic.h"
#include "../../server.h"
#include "../queryoptimizercursor.h"

namespace mongo {

    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    const int MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

    bool runCommands(const char *ns, BSONObj& jsobj, CurOp& curop, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        try {
            return _runCommands(ns, jsobj, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch( SendStaleConfigException& ){
            throw;
        }
        catch ( AssertionException& e ) {
            verify( e.getCode() != SendStaleConfigCode && e.getCode() != RecvStaleConfigCode );

            e.getInfo().append( anObjBuilder , "assertion" , "assertionCode" );
            curop.debug().exceptionInfo = e.getInfo();
        }
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.appendBuf((void*) x.objdata(), x.objsize());
        return true;
    }


    BSONObj id_obj = fromjson("{\"_id\":1}");
    BSONObj empty_obj = fromjson("{}");


    //int dump = 0;

    /* empty result for error conditions */
    QueryResult* emptyMoreResult(long long cursorid) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        QueryResult *qr = (QueryResult *) b.buf();
        qr->cursorId = 0; // 0 indicates no more data to retrieve.
        qr->startingFrom = 0;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->initializeResultFlags();
        qr->nReturned = 0;
        b.decouple();
        return qr;
    }

    QueryResult* processGetMore(const char *ns, int ntoreturn, long long cursorid , CurOp& curop, int pass, bool& exhaust ) {
        exhaust = false;
        ClientCursor::Pin p(cursorid);
        ClientCursor *client_cursor = p.c();

        int bufSize = 512 + sizeof( QueryResult ) + MaxBytesToReturnToClientAtOnce;

        BufBuilder b( bufSize );
        b.skip(sizeof(QueryResult));
        int resultFlags = ResultFlag_AwaitCapable;
        int start = 0;
        int n = 0;

        if ( unlikely(!client_cursor) ) {
            LOGSOME << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = ResultFlag_CursorNotFound;
        }
        else {
            // check for spoofing of the ns such that it does not match the one originally there for the cursor
            uassert(14833, "auth error", str::equals(ns, client_cursor->ns().c_str()));
            uassert(16784, "oplog cursor reading data that is too old", !client_cursor->lastOpForSlaveTooOld());

            int queryOptions = client_cursor->queryOptions();
            TokuCommandSettings settings;
            settings.setBulkFetch(true);
            settings.setQueryCursorMode(DEFAULT_LOCK_CURSOR);
            settings.setCappedAppendPK(queryOptions & QueryOption_AddHiddenPK);
            cc().setTokuCommandSettings(settings);

            verify(client_cursor->transactions.get() != NULL);
            Client::WithTxnStack wts(client_cursor->transactions);

            if (pass == 0) {
                client_cursor->updateSlaveLocation( curop );
            }
            
            curop.debug().query = client_cursor->query();

            start = client_cursor->pos();
            Cursor *c = client_cursor->c();
            BSONObj last;

            // This manager may be stale, but it's the state of chunking when the cursor was created.
            ShardChunkManagerPtr manager = client_cursor->getChunkManager();

            while ( 1 ) {
                if ( !c->ok() ) {
                    if ( c->tailable() ) {
                        /* when a tailable cursor hits "EOF", ok() goes false, and current() is null.  however
                           advance() can still be retries as a reactivation attempt.  when there is new data, it will
                           return true.  that's what we are doing here.
                           */
                        if ( c->advance() )
                            continue;

                        if( n == 0 && (queryOptions & QueryOption_AwaitData) && pass < 1000 ) {
                            return 0;
                        }

                        break;
                    }
                    p.release();

                    // Done with this cursor, steal transaction stack back to commit or abort it here.
                    bool ok = ClientCursor::erase(cursorid);
                    verify(ok);
                    cursorid = 0;
                    client_cursor = 0;
                    break;
                }

                MatchDetails details;
                if ( client_cursor->fields && client_cursor->fields->getArrayOpType() == Projection::ARRAY_OP_POSITIONAL ) {
                    // field projection specified, and contains an array operator
                    details.requestElemMatchKey();
                }

                // in some cases (clone collection) there won't be a matcher
                if ( !c->currentMatches( &details ) ) {
                }
                else if ( manager && ! manager->belongsToMe( client_cursor ) ){
                    LOG(2) << "cursor skipping document in un-owned chunk: " << c->current() << endl;
                }
                else {
                    if( c->getsetdup(c->currPK()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        // save this so that at the end of the loop,
                        // we can update the location for write concern
                        // in replication. Note that if this cursor is not
                        // doing replication, this is pointless
                        if ( client_cursor->queryOptions() & QueryOption_OplogReplay ) {
                            last = c->current();
                        }
                        n++;

                        client_cursor->fillQueryResultFromObj( b, &details );

                        if ( ( ntoreturn && n >= ntoreturn ) || b.len() > MaxBytesToReturnToClientAtOnce ) {
                            c->advance();
                            client_cursor->incPos( n );
                            break;
                        }
                    }
                }
                c->advance();
            }
            
            if ( client_cursor ) {
                if ( client_cursor->queryOptions() & QueryOption_OplogReplay ) {
                    client_cursor->storeOpForSlave( last );
                }
                exhaust = client_cursor->queryOptions() & QueryOption_Exhaust;
            } else {
                // We're done with this transaction, commit it and release it back to the Client.
                cc().commitTopTxn();
                wts.release();
            }
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->_resultFlags() = resultFlags;
        qr->cursorId = cursorid;
        qr->startingFrom = start;
        qr->nReturned = n;
        b.decouple();

        return qr;
    }

    ExplainRecordingStrategy::ExplainRecordingStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo ) :
    _ancillaryInfo( ancillaryInfo ) {
    }

    shared_ptr<ExplainQueryInfo> ExplainRecordingStrategy::doneQueryInfo() {
        shared_ptr<ExplainQueryInfo> ret = _doneQueryInfo();
        ret->setAncillaryInfo( _ancillaryInfo );
        return ret;
    }
    
    NoExplainStrategy::NoExplainStrategy() :
    ExplainRecordingStrategy( ExplainQueryInfo::AncillaryInfo() ) {
    }

    shared_ptr<ExplainQueryInfo> NoExplainStrategy::_doneQueryInfo() {
        verify( false );
        return shared_ptr<ExplainQueryInfo>();
    }
    
    MatchCountingExplainStrategy::MatchCountingExplainStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo ) :
    ExplainRecordingStrategy( ancillaryInfo ),
    _orderedMatches() {
    }
    
    void MatchCountingExplainStrategy::noteIterate( bool match, bool orderedMatch,
                                                   bool loadedRecord, bool chunkSkip ) {
        _noteIterate( match, orderedMatch, loadedRecord, chunkSkip );
        if ( orderedMatch ) {
            ++_orderedMatches;
        }
    }
    
    SimpleCursorExplainStrategy::SimpleCursorExplainStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
     const shared_ptr<Cursor> &cursor ) :
    MatchCountingExplainStrategy( ancillaryInfo ),
    _cursor( cursor ),
    _explainInfo( new ExplainSinglePlanQueryInfo() ) {
    }
 
    void SimpleCursorExplainStrategy::notePlan( bool scanAndOrder, bool indexOnly ) {
        _explainInfo->notePlan( *_cursor, scanAndOrder, indexOnly );
    }

    void SimpleCursorExplainStrategy::_noteIterate( bool match, bool orderedMatch,
                                                   bool loadedRecord, bool chunkSkip ) {
        _explainInfo->noteIterate( match, loadedRecord, chunkSkip, *_cursor );
    }

    shared_ptr<ExplainQueryInfo> SimpleCursorExplainStrategy::_doneQueryInfo() {
        _explainInfo->noteDone( *_cursor );
        return _explainInfo->queryInfo();
    }

    QueryOptimizerCursorExplainStrategy::QueryOptimizerCursorExplainStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
     const shared_ptr<QueryOptimizerCursor> &cursor ) :
    MatchCountingExplainStrategy( ancillaryInfo ),
    _cursor( cursor ) {
    }
    
    void QueryOptimizerCursorExplainStrategy::_noteIterate( bool match, bool orderedMatch,
                                                           bool loadedRecord, bool chunkSkip ) {
        // Note ordered matches only; if an unordered plan is selected, the explain result will
        // be updated with reviseN().
        _cursor->noteIterate( orderedMatch, loadedRecord, chunkSkip );
    }

    shared_ptr<ExplainQueryInfo> QueryOptimizerCursorExplainStrategy::_doneQueryInfo() {
        return _cursor->explainQueryInfo();
    }

    ResponseBuildStrategy::ResponseBuildStrategy( const ParsedQuery &parsedQuery,
                                                  const shared_ptr<Cursor> &cursor,
                                                  BufBuilder &buf ) :
    _parsedQuery( parsedQuery ),
    _cursor( cursor ),
    _queryOptimizerCursor( dynamic_pointer_cast<QueryOptimizerCursor>( _cursor ) ),
    _buf( buf ) {
    }

    void ResponseBuildStrategy::resetBuf() {
        _buf.reset();
        _buf.skip( sizeof( QueryResult ) );
    }

    BSONObj ResponseBuildStrategy::current( bool allowCovered ) const {
        if ( _parsedQuery.returnKey() ) {
            BSONObjBuilder bob;
            bob.appendKeys( _cursor->indexKeyPattern(), _cursor->currKey() );
            return bob.obj();
        }
        if ( allowCovered ) {
            const Projection::KeyOnly *keyFieldsOnly = _cursor->keyFieldsOnly();
            if ( keyFieldsOnly ) {
                return keyFieldsOnly->hydrate( _cursor->currKey() );
            }
        }
        BSONObj ret = _cursor->current();
        verify( ret.isValid() );
        return ret;
    }

    OrderedBuildStrategy::OrderedBuildStrategy( const ParsedQuery &parsedQuery,
                                               const shared_ptr<Cursor> &cursor,
                                               BufBuilder &buf ) :
    ResponseBuildStrategy( parsedQuery, cursor, buf ),
    _skip( _parsedQuery.getSkip() ),
    _bufferedMatches() {
    }
    
    bool OrderedBuildStrategy::handleMatch( bool &orderedMatch, MatchDetails& details ) {
        if ( _cursor->getsetdup(_cursor->currPK()) ) {
            return orderedMatch = false;
        }
        if ( _skip > 0 ) {
            --_skip;
            return orderedMatch = false;
        }
        // Explain does not obey soft limits, so matches should not be buffered.
        if ( !_parsedQuery.isExplain() ) {
            fillQueryResultFromObj( _buf, _parsedQuery.getFields(),
                                    current( true ), &details);
            ++_bufferedMatches;
        }
        return orderedMatch = true;
    }

    ReorderBuildStrategy* ReorderBuildStrategy::make( const ParsedQuery& parsedQuery,
                                                      const shared_ptr<Cursor>& cursor,
                                                      BufBuilder& buf,
                                                      const QueryPlanSummary& queryPlan ) {
        auto_ptr<ReorderBuildStrategy> ret( new ReorderBuildStrategy( parsedQuery, cursor, buf ) );
        ret->init( queryPlan );
        return ret.release();
    }

    ReorderBuildStrategy::ReorderBuildStrategy( const ParsedQuery &parsedQuery,
                                               const shared_ptr<Cursor> &cursor,
                                               BufBuilder &buf ) :
    ResponseBuildStrategy( parsedQuery, cursor, buf ),
    _bufferedMatches() {
    }
    
    void ReorderBuildStrategy::init( const QueryPlanSummary &queryPlan ) {
        _scanAndOrder.reset( newScanAndOrder( queryPlan ) );
    }

    bool ReorderBuildStrategy::handleMatch( bool &orderedMatch, MatchDetails& details ) {
        orderedMatch = false;
        if ( _cursor->getsetdup(_cursor->currPK()) ) {
            return false;
        }
        _handleMatchNoDedup();
        return true;
    }
    
    void ReorderBuildStrategy::_handleMatchNoDedup() {
        _scanAndOrder->add( current( false ) );
    }

    int ReorderBuildStrategy::rewriteMatches() {
        cc().curop()->debug().scanAndOrder = true;
        int ret = 0;
        _scanAndOrder->fill( _buf, &_parsedQuery, ret );
        _bufferedMatches = ret;
        return ret;
    }
    
    ScanAndOrder *
    ReorderBuildStrategy::newScanAndOrder( const QueryPlanSummary &queryPlan ) const {
        verify( !_parsedQuery.getOrder().isEmpty() );
        verify( _cursor->ok() );
        const FieldRangeSet *fieldRangeSet = 0;
        if ( queryPlan.valid() ) {
            fieldRangeSet = queryPlan._fieldRangeSetMulti.get();
        }
        else {
            verify( _queryOptimizerCursor );
            fieldRangeSet = _queryOptimizerCursor->initialFieldRangeSet();
        }
        verify( fieldRangeSet );
        return new ScanAndOrder( _parsedQuery.getSkip(),
                                _parsedQuery.getNumToReturn(),
                                _parsedQuery.getOrder(),
                                *fieldRangeSet );
    }

    HybridBuildStrategy* HybridBuildStrategy::make( const ParsedQuery& parsedQuery,
                                                    const shared_ptr<QueryOptimizerCursor>& cursor,
                                                    BufBuilder& buf ) {
        auto_ptr<HybridBuildStrategy> ret( new HybridBuildStrategy( parsedQuery, cursor, buf ) );
        ret->init();
        return ret.release();
    }

    HybridBuildStrategy::HybridBuildStrategy( const ParsedQuery &parsedQuery,
                                             const shared_ptr<QueryOptimizerCursor> &cursor,
                                             BufBuilder &buf ) :
    ResponseBuildStrategy( parsedQuery, cursor, buf ),
    _orderedBuild( _parsedQuery, _cursor, _buf ),
    _reorderedMatches() {
    }

    void HybridBuildStrategy::init() {
        _reorderBuild.reset( ReorderBuildStrategy::make( _parsedQuery, _cursor, _buf,
                                                         QueryPlanSummary() ) );
    }

    bool HybridBuildStrategy::handleMatch( bool &orderedMatch, MatchDetails& details ) {
        if ( !_queryOptimizerCursor->currentPlanScanAndOrderRequired() ) {
            return _orderedBuild.handleMatch( orderedMatch, details );
        }
        orderedMatch = false;
        return handleReorderMatch();
    }
    
    bool HybridBuildStrategy::handleReorderMatch() {
        if ( _scanAndOrderDups.getsetdup(_cursor->currPK()) ) {
            return false;
        }
        try {
            _reorderBuild->_handleMatchNoDedup();
        } catch ( const UserException &e ) {
            if ( e.getCode() == ScanAndOrderMemoryLimitExceededAssertionCode ) {
                if ( _queryOptimizerCursor->hasPossiblyExcludedPlans() ) {
                    _queryOptimizerCursor->clearIndexesForPatterns();
                    throw QueryRetryException();
                }
                else if ( _queryOptimizerCursor->runningInitialInOrderPlan() ) {
                    _queryOptimizerCursor->abortOutOfOrderPlans();
                    return true;
                }
            }
            throw;
        }
        return true;
    }
    
    int HybridBuildStrategy::rewriteMatches() {
        if ( !_queryOptimizerCursor->completePlanOfHybridSetScanAndOrderRequired() ) {
            return _orderedBuild.rewriteMatches();
        }
        _reorderedMatches = true;
        resetBuf();
        return _reorderBuild->rewriteMatches();
    }

    int HybridBuildStrategy::bufferedMatches() const {
        return _reorderedMatches ?
                _reorderBuild->bufferedMatches() :
                _orderedBuild.bufferedMatches();
    }

    void HybridBuildStrategy::finishedFirstBatch() {
        _queryOptimizerCursor->abortOutOfOrderPlans();
    }
    
    QueryResponseBuilder *QueryResponseBuilder::make( const ParsedQuery &parsedQuery,
                                                     const shared_ptr<Cursor> &cursor,
                                                     const QueryPlanSummary &queryPlan,
                                                     const BSONObj &oldPlan ) {
        auto_ptr<QueryResponseBuilder> ret( new QueryResponseBuilder( parsedQuery, cursor ) );
        ret->init( queryPlan, oldPlan );
        return ret.release();
    }
    
    QueryResponseBuilder::QueryResponseBuilder( const ParsedQuery &parsedQuery,
                                               const shared_ptr<Cursor> &cursor ) :
    _parsedQuery( parsedQuery ),
    _cursor( cursor ),
    _queryOptimizerCursor( dynamic_pointer_cast<QueryOptimizerCursor>( _cursor ) ),
    _buf( 32768 ) { // TODO be smarter here
    }
    
    void QueryResponseBuilder::init( const QueryPlanSummary &queryPlan, const BSONObj &oldPlan ) {
        _chunkManager = newChunkManager();
        _explain = newExplainRecordingStrategy( queryPlan, oldPlan );
        _builder = newResponseBuildStrategy( queryPlan );
        _builder->resetBuf();
    }

    bool QueryResponseBuilder::addMatch() {
        MatchDetails details;

        if ( _parsedQuery.getFields() && _parsedQuery.getFields()->getArrayOpType() == Projection::ARRAY_OP_POSITIONAL ) {
            // field projection specified, and contains an array operator
            details.requestElemMatchKey();
        }

        if ( !currentMatches( details ) ) {
            return false;
        }
        if ( !chunkMatches() ) {
            return false;
        }
        bool orderedMatch = false;
        bool match = _builder->handleMatch( orderedMatch, details );
        _explain->noteIterate( match, orderedMatch, true, false );
        return match;
    }

    bool QueryResponseBuilder::enoughForFirstBatch() const {
        return _parsedQuery.enoughForFirstBatch( _builder->bufferedMatches(), _buf.len() );
    }

    bool QueryResponseBuilder::enoughTotalResults() const {
        if ( _parsedQuery.isExplain() ) {
            return _parsedQuery.enoughForExplain( _explain->orderedMatches() );
        }
        return ( _parsedQuery.enough( _builder->bufferedMatches() ) ||
                _buf.len() >= MaxBytesToReturnToClientAtOnce );
    }

    void QueryResponseBuilder::finishedFirstBatch() {
        _builder->finishedFirstBatch();
    }

    int QueryResponseBuilder::handoff( Message &result ) {
        int rewriteCount = _builder->rewriteMatches();
        if ( _parsedQuery.isExplain() ) {
            shared_ptr<ExplainQueryInfo> explainInfo = _explain->doneQueryInfo();
            if ( rewriteCount != -1 ) {
                explainInfo->reviseN( rewriteCount );
            }
            _builder->resetBuf();
            fillQueryResultFromObj( _buf, 0, explainInfo->bson() );
            result.appendData( _buf.buf(), _buf.len() );
            _buf.decouple();
            return 1;
        }
        if ( _buf.len() > 0 ) {
            result.appendData( _buf.buf(), _buf.len() );
            _buf.decouple();
        }
        return _builder->bufferedMatches();
    }

    ShardChunkManagerPtr QueryResponseBuilder::newChunkManager() const {
        if ( !shardingState.needShardChunkManager( _parsedQuery.ns() ) ) {
            return ShardChunkManagerPtr();
        }
        return shardingState.getShardChunkManager( _parsedQuery.ns() );
    }

    shared_ptr<ExplainRecordingStrategy> QueryResponseBuilder::newExplainRecordingStrategy
    ( const QueryPlanSummary &queryPlan, const BSONObj &oldPlan ) const {
        if ( !_parsedQuery.isExplain() ) {
            return shared_ptr<ExplainRecordingStrategy>( new NoExplainStrategy() );
        }
        ExplainQueryInfo::AncillaryInfo ancillaryInfo;
        ancillaryInfo._oldPlan = oldPlan;
        if ( _queryOptimizerCursor ) {
            return shared_ptr<ExplainRecordingStrategy>
            ( new QueryOptimizerCursorExplainStrategy( ancillaryInfo, _queryOptimizerCursor ) );
        }
        shared_ptr<ExplainRecordingStrategy> ret
        ( new SimpleCursorExplainStrategy( ancillaryInfo, _cursor ) );
        ret->notePlan( queryPlan.valid() && queryPlan._scanAndOrderRequired,
                      queryPlan._keyFieldsOnly );
        return ret;
    }

    shared_ptr<ResponseBuildStrategy> QueryResponseBuilder::newResponseBuildStrategy
    ( const QueryPlanSummary &queryPlan ) {
        bool unordered = _parsedQuery.getOrder().isEmpty();
        bool empty = !_cursor->ok();
        bool singlePlan = !_queryOptimizerCursor;
        bool singleOrderedPlan =
        singlePlan && ( !queryPlan.valid() || !queryPlan._scanAndOrderRequired );
        CandidatePlanCharacter queryOptimizerPlans;
        if ( _queryOptimizerCursor ) {
            queryOptimizerPlans = _queryOptimizerCursor->initialCandidatePlans();
        }
        if ( unordered ||
            empty ||
            singleOrderedPlan ||
            ( !singlePlan && !queryOptimizerPlans.mayRunOutOfOrderPlan() ) ) {
            return shared_ptr<ResponseBuildStrategy>
            ( new OrderedBuildStrategy( _parsedQuery, _cursor, _buf ) );
        }
        if ( singlePlan ||
            !queryOptimizerPlans.mayRunInOrderPlan() ) {
            return shared_ptr<ResponseBuildStrategy>
            ( ReorderBuildStrategy::make( _parsedQuery, _cursor, _buf, queryPlan ) );
        }
        return shared_ptr<ResponseBuildStrategy>
        ( HybridBuildStrategy::make( _parsedQuery, _queryOptimizerCursor, _buf ) );
    }

    bool QueryResponseBuilder::currentMatches( MatchDetails& details ) {
        if ( _cursor->currentMatches( &details ) ) {
            return true;
        }
        _explain->noteIterate( false, false, details.hasLoadedRecord(), false );
        return false;
    }

    bool QueryResponseBuilder::chunkMatches() {
        if ( !_chunkManager ) {
            return true;
        }
        // TODO: should make this covered at some point
        if ( _chunkManager->belongsToMe( _cursor->current() ) ) {
            return true;
        }
        _explain->noteIterate( false, false, true, true );
        return false;
    }
    
    /**
     * Run a query with a cursor provided by the query optimizer, or FindingStartCursor.
     * @yields the db lock.
     * @returns true if client cursor was saved, false if the query has completed.
     */
    bool queryWithQueryOptimizer( int queryOptions, const string& ns,
                                  const BSONObj &jsobj, CurOp& curop,
                                  const BSONObj &query, const BSONObj &order,
                                  const shared_ptr<ParsedQuery> &pq_shared,
                                  const ConfigVersion &shardingVersionAtStart,
                                  const bool getCachedExplainPlan,
                                  Client::Transaction &txn,
                                  Message &result ) {

        const ParsedQuery &pq( *pq_shared );
        shared_ptr<Cursor> cursor;
        QueryPlanSummary queryPlan;

        const bool tailable = pq.hasOption( QueryOption_CursorTailable ) && pq.getNumToReturn() != 1;
        
        LOG(1) << "query beginning read-only transaction. tailable: " << tailable << endl;
        
        BSONObj oldPlan;
        if (getCachedExplainPlan) {
            scoped_ptr<MultiPlanScanner> mps( MultiPlanScanner::make( ns.c_str(), query, order ) );
            oldPlan = mps->cachedPlanExplainSummary();
        }
        
        cursor =
            NamespaceDetailsTransient::getCursor( ns.c_str(), query, order, QueryPlanSelectionPolicy::any(),
                                                  0, pq_shared, false, &queryPlan );
        verify( cursor );

        // Tailable cursors must be marked as such before any use. This is so that
        // the implementation knows that uncommitted data cannot be returned.
        if ( tailable ) {
            cursor->setTailable();
        }

        scoped_ptr<QueryResponseBuilder> queryResponseBuilder
                ( QueryResponseBuilder::make( pq, cursor, queryPlan, oldPlan ) );
        bool saveClientCursor = false;
        int options = QueryOption_NoCursorTimeout;
        if (pq.hasOption( QueryOption_OplogReplay )) {
            options |= QueryOption_OplogReplay;
        }
        ClientCursor::Holder ccPointer( new ClientCursor( options, cursor, ns ) );

        // for oplog cursors, we check if we are reading data that is too old and might
        // be stale.
        bool opChecked = false;
        bool slaveLocationUpdated = false;
        BSONObj last;
        for ( ; cursor->ok(); cursor->advance() ) {

            if ( pq.getMaxScan() && cursor->nscanned() > pq.getMaxScan() ) {
                break;
            }
            
            if ( !queryResponseBuilder->addMatch() ) {
                continue;
            }
            
            // Note slave's position in the oplog.
            if ( pq.hasOption( QueryOption_OplogReplay ) ) {
                BSONObj current = cursor->current();
                last = current;
                
                // the first row returned is equal to the last element that
                // the slave has synced up to, so we might as well update
                // the slave location
                if (!slaveLocationUpdated) {
                    ccPointer->storeOpForSlave(current);
                    ccPointer->updateSlaveLocation(curop);
                    slaveLocationUpdated = true;
                }
                // check if data we are about to return may be too stale
                if (!opChecked) {
                    uint64_t ts = current["ts"]._numberLong();
                    uassert(16785, "oplog cursor reading data that is too old", ts);
                    opChecked = true;
                }
            }
            
            if ( !cursor->supportGetMore() || pq.isExplain() ) {
                if ( queryResponseBuilder->enoughTotalResults() ) {
                    break;
                }
            }
            else if ( queryResponseBuilder->enoughForFirstBatch() ) {
                // if only 1 requested, no cursor saved for efficiency...we assume it is findOne()
                if ( pq.wantMore() && pq.getNumToReturn() != 1 ) {
                    queryResponseBuilder->finishedFirstBatch();
                    if ( cursor->advance() ) {
                        saveClientCursor = true;
                    }
                }
                break;
            }
        }

        // If the tailing request succeeded
        if ( cursor->tailable() ) {
            saveClientCursor = true;
        }
        
        if ( ! shardingState.getVersion( ns ).isWriteCompatibleWith( shardingVersionAtStart ) ) {
            // if the version changed during the query
            // we might be missing some data
            // and its safe to send this as mongos can resend
            // at this point
            throw SendStaleConfigException( ns , "version changed during initial query", shardingVersionAtStart, shardingState.getVersion( ns ) );
        }
        
        int nReturned = queryResponseBuilder->handoff( result );

        ccPointer.reset();
        long long cursorid = 0;
        if ( saveClientCursor ) {
            // Create a new ClientCursor, with a default timeout.
            ccPointer.reset( new ClientCursor( queryOptions, cursor, ns,
                                              jsobj.getOwned() ) );
            cursorid = ccPointer->cursorid();
            DEV tlog(2) << "query has more, cursorid: " << cursorid << endl;
            
            if ( !ccPointer->ok() && ccPointer->c()->tailable() ) {
                DEV tlog() << "query has no more but tailable, cursorid: " << cursorid << endl;
            }
            
            if( queryOptions & QueryOption_Exhaust ) {
                curop.debug().exhaust = true;
            }
            
            // Set attributes for getMore.
            ccPointer->setChunkManager( queryResponseBuilder->chunkManager() );
            ccPointer->setPos( nReturned );
            ccPointer->pq = pq_shared;
            ccPointer->fields = pq.getFieldPtr();

            if (pq.hasOption( QueryOption_OplogReplay )) {
                ccPointer->storeOpForSlave(last);
            }
            // Clones the transaction and hand's off responsibility
            // of its completion to the client cursor's destructor.
            cc().swapTransactionStack(ccPointer->transactions);
            ccPointer.release();
        }

        QueryResult *qr = (QueryResult *) result.header();
        qr->cursorId = cursorid;
        curop.debug().cursorid = ( cursorid == 0 ? -1 : qr->cursorId );
        qr->setResultFlagsToOk();
        // qr->len is updated automatically by appendData()
        curop.debug().responseLength = qr->len;
        qr->setOperation(opReply);
        qr->startingFrom = 0;
        qr->nReturned = nReturned;

        int duration = curop.elapsedMillis();
        bool dbprofile = curop.shouldDBProfile( duration );
        if ( dbprofile || duration >= cmdLine.slowMS ) {
            curop.debug().nscanned = cursor->nscanned();
            curop.debug().ntoskip = pq.getSkip();
        }
        curop.debug().nreturned = nReturned;

        return saveClientCursor;
    }

    bool queryIdHack( const char* ns, const BSONObj& query, const ParsedQuery& pq, CurOp& curop, Message& result ) {
        int n = 0;
        auto_ptr< QueryResult > qr;
        BSONObj resObject;

        bool found = false;
        {
            TokuCommandSettings settings;
            settings.setQueryCursorMode(DEFAULT_LOCK_CURSOR);
            settings.setCappedAppendPK(pq.hasOption(QueryOption_AddHiddenPK));
            cc().setTokuCommandSettings(settings);
            Client::ReadContext ctx(ns);
            Client::Transaction transaction(DB_TXN_SNAPSHOT | DB_TXN_READ_ONLY);
            replVerifyReadsOk(&pq);

            NamespaceDetails *d = nsdetails(ns);
            if (d != NULL) {
                if (!d->mayFindById()) {
                    // we have to resort to using the optimizer
                    return false;
                }
                found = d->findById( query, resObject );
                if (found) {
                    transaction.commit();
                }
            }
        }

        if ( shardingState.needShardChunkManager( ns ) ) {
            ShardChunkManagerPtr m = shardingState.getShardChunkManager( ns );
            if ( m && ! m->belongsToMe( resObject ) ) {
                // I have something for this _id
                // but it doesn't belong to me
                // so return nothing
                resObject = BSONObj();
                found = false;
            }
        }

        BufBuilder bb(sizeof(QueryResult)+resObject.objsize()+32);
        bb.skip(sizeof(QueryResult));

        curop.debug().idhack = true;
        if ( found ) {
            n = 1;
            fillQueryResultFromObj( bb , pq.getFields() , resObject );
        }

        qr.reset( (QueryResult *) bb.buf() );
        bb.decouple();
        qr->setResultFlagsToOk();
        qr->len = bb.len();

        curop.debug().responseLength = bb.len();
        qr->setOperation(opReply);
        qr->cursorId = 0;
        qr->startingFrom = 0;
        qr->nReturned = n;

        result.setData( qr.release(), true );
        return true;
    }

    static string lockedRunQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result, 
                                 shared_ptr<ParsedQuery>& pq_shared, ParsedQuery& pq, bool hasRetried) {
        BSONObj jsobj = q.query;
        int queryOptions = q.queryOptions;
        const char *ns = q.ns;

        bool explain = pq.isExplain();
        BSONObj order = pq.getOrder();
        BSONObj query = pq.getFilter();

        const bool tailable = pq.hasOption( QueryOption_CursorTailable ) && pq.getNumToReturn() != 1;
        Client::Transaction transaction((tailable ? DB_READ_UNCOMMITTED : DB_TXN_SNAPSHOT) | DB_TXN_READ_ONLY);    
        const ConfigVersion shardingVersionAtStart = shardingState.getVersion( ns );
                
        replVerifyReadsOk(&pq);
                
        if ( pq.hasOption( QueryOption_CursorTailable ) ) {
            NamespaceDetails *d = nsdetails( ns );
            if (d != NULL && !(d->isCapped() || str::equals(ns, rsoplog))) {
                uasserted( 13051, "tailable cursor requested on non-capped, non-oplog collection" );
            }
            const BSONObj nat1 = BSON( "$natural" << 1 );
            if ( order.isEmpty() ) {
                order = nat1;
            } else {
                uassert( 13052, "only {$natural:1} order allowed for tailable cursor", order == nat1 );
            }
        }
            
        // Run a regular query.

        const bool getCachedExplainPlan = ! hasRetried && explain && ! pq.hasIndexSpecifier();
        const bool savedCursor = queryWithQueryOptimizer( queryOptions, ns, jsobj, curop, query,
                                                          order, pq_shared, shardingVersionAtStart,
                                                          getCachedExplainPlan, transaction, result );
        // Did not save the cursor, so we can commit the transaction now.
        if (!savedCursor) {
            transaction.commit();
        }
        return curop.debug().exhaust ? ns : "";
    }

    /**
     * Run a query -- includes checking for and running a Command.
     * @return points to ns if exhaust mode. 0=normal mode
     * @locks the db mutex for reading (and potentially for writing temporarily to create a new db).
     * @yields the db mutex periodically after acquiring it.
     * @asserts on scan and order memory exhaustion and other cases.
     */
    string runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result) {
        shared_ptr<ParsedQuery> pq_shared( new ParsedQuery(q) );
        ParsedQuery& pq( *pq_shared );
        BSONObj jsobj = q.query;
        int queryOptions = q.queryOptions;
        const char *ns = q.ns;

        uassert( 16332 , "can't have an empty ns" , ns[0] );

        if( logLevel >= 2 )
            log() << "runQuery called " << ns << " " << jsobj << endl;

        curop.debug().ns = ns;
        curop.debug().ntoreturn = pq.getNumToReturn();
        curop.debug().query = jsobj;
        curop.setQuery(jsobj);

        const NamespaceString nsString( ns ); // TODO: Use a c string for speed
        uassert( 16256, str::stream() << "Invalid ns [" << ns << "]", nsString.isValid() );

        // Run a command.
        
        if ( pq.couldBeCommand() ) {
            curop.markCommand();
            BufBuilder bb;
            bb.skip(sizeof(QueryResult));
            BSONObjBuilder cmdResBuf;
            if ( runCommands(ns, jsobj, curop, bb, cmdResBuf, false, queryOptions) ) {
                curop.debug().iscommand = true;
                curop.debug().query = jsobj;

                auto_ptr< QueryResult > qr;
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                curop.debug().responseLength = bb.len();
                qr->setOperation(opReply);
                qr->cursorId = 0;
                qr->startingFrom = 0;
                qr->nReturned = 1;
                result.setData( qr.release(), true );
            }
            else {
                uasserted(13530, "bad or malformed command request?");
            }
            return "";
        }

        bool explain = pq.isExplain();
        BSONObj order = pq.getOrder();
        BSONObj query = pq.getFilter();

        /* The ElemIter will not be happy if this isn't really an object. So throw exception
           here when that is true.
           (Which may indicate bad data from client.)
        */
        if ( query.objsize() == 0 ) {
            out() << "Bad query object?\n  jsobj:";
            out() << jsobj.toString() << "\n  query:";
            out() << query.toString() << endl;
            uassert( 10110 , "bad query object", false);
        }


        // Run a simple id query.

        // - Don't do it for explains.
        // - Don't do it for tailable cursors.
        if ( !explain && isSimpleIdQuery( query ) && !pq.hasOption( QueryOption_CursorTailable ) ) {
            if ( queryIdHack( ns, query, pq, curop, result ) ) {
                return "";
            }
        }

        // sanity check the query and projection
        if ( pq.getFields() != NULL )
            pq.getFields()->validateQuery( query );

        // these now may stored in a ClientCursor or somewhere else,
        // so make sure we use a real copy
        jsobj = jsobj.getOwned();
        query = query.getOwned();
        order = order.getOwned();

        // Tailable cursors need to read newly written entries from the tail
        // of the collection. They manually arbitrate with the collection over
        // what data is readable and when, so we choose read uncommited isolation.
        TokuCommandSettings settings;
        settings.setQueryCursorMode(DEFAULT_LOCK_CURSOR);
        settings.setBulkFetch(true);
        settings.setCappedAppendPK(pq.hasOption(QueryOption_AddHiddenPK));
        cc().setTokuCommandSettings(settings);

        bool hasRetried = false;
        while ( 1 ) {
            try {
                Client::ReadContext ctx(ns);
                return lockedRunQuery(m, q, curop, result, pq_shared, pq, hasRetried);
            }
            catch ( const QueryRetryException & ) {
                // In some cases the query may be retried if there is an in memory sort size assertion.
                verify( ! hasRetried );
                hasRetried = true;
            }
            catch (RetryWithWriteLock & ) {
                log() << "retry " << ns << endl;
                Client::WriteContext ctx(ns);
                return lockedRunQuery(m, q, curop, result, pq_shared, pq, hasRetried);
            }
        }
    }
} // namespace mongo
