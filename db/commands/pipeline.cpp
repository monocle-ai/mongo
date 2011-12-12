/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "db/commands/pipeline.h"

#include "db/cursor.h"
#include "db/pipeline/accumulator.h"
#include "db/pipeline/document.h"
#include "db/pipeline/document_source.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/expression_context.h"
#include "db/pdfile.h"

namespace mongo {

    const char Pipeline::commandName[] = "aggregate";
    const char Pipeline::pipelineName[] = "pipeline";
    const char Pipeline::fromRouterName[] = "fromRouter";
    const char Pipeline::splitMongodPipelineName[] = "splitMongodPipeline";

    Pipeline::~Pipeline() {
    }

    Pipeline::Pipeline(const intrusive_ptr<ExpressionContext> &pTheCtx):
	collectionName(),
	sourceVector(),
        splitMongodPipeline(DEBUG_BUILD == 1), /* test: always split for DEV */
        pCtx(pTheCtx) {
    }



    /* this structure is used to make a lookup table of operators */
    struct StageDesc {
	const char *pName;
	intrusive_ptr<DocumentSource> (*pFactory)(
	    BSONElement *, const intrusive_ptr<ExpressionContext> &);
    };

    /* this table must be in alphabetical order by name for bsearch() */
    static const StageDesc stageDesc[] = {
#ifdef NEVER /* disabled for now in favor of $match */
	{DocumentSourceFilter::filterName,
	 DocumentSourceFilter::createFromBson},
#endif
	{DocumentSourceGroup::groupName,
	 DocumentSourceGroup::createFromBson},
	{DocumentSourceLimit::limitName,
	 DocumentSourceLimit::createFromBson},
	{DocumentSourceMatch::matchName,
	 DocumentSourceMatch::createFromBson},
#ifdef LATER /* https://jira.mongodb.org/browse/SERVER-3253 */
	{DocumentSourceOut::outName,
	 DocumentSourceOut::createFromBson},
#endif
	{DocumentSourceProject::projectName,
	 DocumentSourceProject::createFromBson},
	{DocumentSourceSkip::skipName,
	 DocumentSourceSkip::createFromBson},
	{DocumentSourceSort::sortName,
	 DocumentSourceSort::createFromBson},
	{DocumentSourceUnwind::unwindName,
	 DocumentSourceUnwind::createFromBson},
    };
    static const size_t nStageDesc = sizeof(stageDesc) / sizeof(StageDesc);

    static int stageDescCmp(const void *pL, const void *pR) {
	return strcmp(((const StageDesc *)pL)->pName,
		      ((const StageDesc *)pR)->pName);
    }

    boost::shared_ptr<Pipeline> Pipeline::parseCommand(
	string &errmsg, BSONObj &cmdObj,
	const intrusive_ptr<ExpressionContext> &pCtx) {
	boost::shared_ptr<Pipeline> pPipeline(new Pipeline(pCtx));
        vector<BSONElement> pipeline;

        /* gather the specification for the aggregation */
        for(BSONObj::iterator cmdIterator = cmdObj.begin();
                cmdIterator.more(); ) {
            BSONElement cmdElement(cmdIterator.next());
            const char *pFieldName = cmdElement.fieldName();

            /* look for the aggregation command */
            if (!strcmp(pFieldName, commandName)) {
                pPipeline->collectionName = cmdElement.String();
                continue;
            }

            /* check for the collection name */
            if (!strcmp(pFieldName, pipelineName)) {
                pipeline = cmdElement.Array();
                continue;
            }

	    /* if the request came from the router, we're in a shard */
	    if (!strcmp(pFieldName, fromRouterName)) {
		pCtx->setInShard(cmdElement.Bool());
		continue;
	    }

	    /* check for debug options */
	    if (!strcmp(pFieldName, splitMongodPipelineName)) {
		pPipeline->splitMongodPipeline = true;
		continue;
	    }

            /* we didn't recognize a field in the command */
            ostringstream sb;
            sb <<
               "Pipeline::parseCommand(): unrecognized field \"" <<
               cmdElement.fieldName();
            errmsg = sb.str();
	    return boost::shared_ptr<Pipeline>();
        }

        /*
          If we get here, we've harvested the fields we expect for a pipeline.

          Set up the specified document source pipeline.
        */
	SourceVector *pSourceVector = &pPipeline->sourceVector; // shorthand

        /* iterate over the steps in the pipeline */
        const size_t nSteps = pipeline.size();
        for(size_t iStep = 0; iStep < nSteps; ++iStep) {
            /* pull out the pipeline element as an object */
            BSONElement pipeElement(pipeline[iStep]);
	    uassert(15942, str::stream() << "pipeline element " <<
		    iStep << " is not an object",
		    pipeElement.type() == Object);
            BSONObj bsonObj(pipeElement.Obj());

	    intrusive_ptr<DocumentSource> pSource;

            /* use the object to add a DocumentSource to the processing chain */
            BSONObjIterator bsonIterator(bsonObj);
            while(bsonIterator.more()) {
                BSONElement bsonElement(bsonIterator.next());
                const char *pFieldName = bsonElement.fieldName();

                /* select the appropriate operation and instantiate */
		StageDesc key;
		key.pName = pFieldName;
		const StageDesc *pDesc = (const StageDesc *)
		    bsearch(&key, stageDesc, nStageDesc, sizeof(StageDesc),
			    stageDescCmp);
		if (pDesc)
		    pSource = (*pDesc->pFactory)(&bsonElement, pCtx);
                else {
                    ostringstream sb;
                    sb <<
                       "Pipeline::run(): unrecognized pipeline op \"" <<
                       pFieldName;
                    errmsg = sb.str();
		    return shared_ptr<Pipeline>();
                }
            }

	    pSourceVector->push_back(pSource);
        }

	/* if there aren't any pipeline stages, there's nothing more to do */
	if (!pSourceVector->size())
	    return pPipeline;

	/*
	  Move filters up where possible.

	  CW TODO -- move filter past projections where possible, and noting
	  corresponding field renaming.
	*/

	/*
	  Wherever there is a match immediately following a sort, swap them.
	  This means we sort fewer items.  Neither changes the documents in
	  the stream, so this transformation shouldn't affect the result.

	  We do this first, because then when we coalesce operators below,
	  any adjacent matches will be combined.
	 */
	for(size_t srcn = pSourceVector->size(), srci = 1;
	    srci < srcn; ++srci) {
	    intrusive_ptr<DocumentSource> &pSource = pSourceVector->at(srci);
	    if (dynamic_cast<DocumentSourceMatch *>(pSource.get())) {
		intrusive_ptr<DocumentSource> &pPrevious =
		    pSourceVector->at(srci - 1);
		if (dynamic_cast<DocumentSourceSort *>(pPrevious.get())) {
		    /* swap this item with the previous */
		    intrusive_ptr<DocumentSource> pTemp(pPrevious);
		    pPrevious = pSource;
		    pSource = pTemp;
		}
	    }
	}

	/*
	  Coalesce adjacent filters where possible.  Two adjacent filters
	  are equivalent to one filter whose predicate is the conjunction of
	  the two original filters' predicates.  For now, capture this by
	  giving any DocumentSource the option to absorb it's successor; this
	  will also allow adjacent projections to coalesce when possible.

	  Run through the DocumentSources, and give each one the opportunity
	  to coalesce with its successor.  If successful, remove the
	  successor.

	  Move all document sources to a temporary list.
	*/
	SourceVector tempVector(*pSourceVector);
	pSourceVector->clear();

	/* move the first one to the final list */
	pSourceVector->push_back(tempVector[0]);

	/* run through the sources, coalescing them or keeping them */
	for(size_t tempn = tempVector.size(), tempi = 1;
	    tempi < tempn; ++tempi) {
	    /*
	      If we can't coalesce the source with the last, then move it
	      to the final list, and make it the new last.  (If we succeeded,
	      then we're still on the same last, and there's no need to move
	      or do anything with the source -- the destruction of tempVector
	      will take care of the rest.)
	    */
	    intrusive_ptr<DocumentSource> &pLastSource = pSourceVector->back();
	    intrusive_ptr<DocumentSource> &pTemp = tempVector.at(tempi);
	    if (!pLastSource->coalesce(pTemp))
		pSourceVector->push_back(pTemp);
	}

	/* optimize the elements in the pipeline */
	for(SourceVector::iterator iter(pSourceVector->begin()),
		listEnd(pSourceVector->end()); iter != listEnd; ++iter)
	    (*iter)->optimize();

	return pPipeline;
    }

    shared_ptr<Pipeline> Pipeline::splitForSharded() {
	/* create an initialize the shard spec we'll return */
	shared_ptr<Pipeline> pShardPipeline(new Pipeline(pCtx));
	pShardPipeline->collectionName = collectionName;

	/* put the source list aside */
	SourceVector tempVector(sourceVector);
	sourceVector.clear();

	/*
	  Run through the pipeline, looking for points to split it into
	  shard pipelines, and the rest.
	 */
	while(!tempVector.empty()) {
	    intrusive_ptr<DocumentSource> &pSource = tempVector.front();

#ifdef MONGODB_SERVER3832 /* see https://jira.mongodb.org/browse/SERVER-3832 */
	    DocumentSourceSort *pSort =
		dynamic_cast<DocumentSourceSort *>(pSource.get());
	    if (pSort) {
		/*
		  There's no point in sorting until the result is combined.
		  Therefore, sorts should be done in mongos, and not in
		  the shard at all.  Add all the remaining operators to
		  the mongos list and quit.

		  TODO:  unless the sort key is the shard key.
		  TODO:  we could also do a merge sort in mongos in the
		  future, and split here.
		*/
		for(size_t tempn = tempVector.size(), tempi = 0;
		    tempi < tempn; ++tempi)
		    sourceVector.push_back(tempVector[tempi]);
		break;
	    }
#endif

	    /* hang on to this in advance, in case it is a group */
	    DocumentSourceGroup *pGroup =
		dynamic_cast<DocumentSourceGroup *>(pSource.get());

	    /* move the source from the tempVector to the shard sourceVector */
	    pShardPipeline->sourceVector.push_back(pSource);
	    tempVector.erase(tempVector.begin());

	    /*
	      If we found a group, that's a split point.
	     */
	    if (pGroup) {
		/* start this pipeline with the group merger */
		sourceVector.push_back(pGroup->createMerger());

		/* and then add everything that remains and quit */
		for(size_t tempn = tempVector.size(), tempi = 0;
		    tempi < tempn; ++tempi)
		    sourceVector.push_back(tempVector[tempi]);
		break;
	    }
	}

	return pShardPipeline;
    }

    void Pipeline::getCursorMods(BSONObjBuilder *pQueryBuilder,
	BSONObjBuilder *pSortBuilder) {
	/* look for an initial $match */
	if (!sourceVector.size())
	    return;
	const intrusive_ptr<DocumentSource> &pMC = sourceVector.front();
	const DocumentSourceMatch *pMatch =
	    dynamic_cast<DocumentSourceMatch *>(pMC.get());

	if (pMatch) {
	    /* build the query */
	    pMatch->toMatcherBson(pQueryBuilder);

	    /* remove the match from the pipeline */
	    sourceVector.erase(sourceVector.begin());
	}

	/* look for an initial $sort */
	if (!sourceVector.size())
	    return;
#ifdef MONGODB_SERVER3832 /* see https://jira.mongodb.org/browse/SERVER-3832 */
	const intrusive_ptr<DocumentSource> &pSC = sourceVector.front();
	const DocumentSourceSort *pSort = 
	    dynamic_cast<DocumentSourceSort *>(pSC.get());

	if (pSort) {
	    /* build the sort key */
	    pSort->sortKeyToBson(pSortBuilder, false);

	    /* remove the sort from the pipeline */
	    sourceVector.erase(sourceVector.begin());
	}
#endif
    }

    void Pipeline::toBson(BSONObjBuilder *pBuilder) const {
	/* create an array out of the pipeline operations */
	BSONArrayBuilder arrayBuilder;
	for(SourceVector::const_iterator iter(sourceVector.begin()),
		listEnd(sourceVector.end()); iter != listEnd; ++iter) {
	    intrusive_ptr<DocumentSource> pSource(*iter);
	    pSource->addToBsonArray(&arrayBuilder);
	}

	/* add the top-level items to the command */
	pBuilder->append(commandName, getCollectionName());
	pBuilder->append(pipelineName, arrayBuilder.arr());

	bool btemp;
	if ((btemp = getSplitMongodPipeline())) {
	    pBuilder->append(splitMongodPipelineName, btemp);
	}
	if ((btemp = pCtx->getInRouter())) {
	    pBuilder->append(fromRouterName, btemp);
	}
    }

    bool Pipeline::run(BSONObjBuilder &result, string &errmsg,
		       intrusive_ptr<DocumentSource> pSource) {
	/* chain together the sources we found */
	for(SourceVector::iterator iter(sourceVector.begin()),
		listEnd(sourceVector.end()); iter != listEnd; ++iter) {
	    intrusive_ptr<DocumentSource> pTemp(*iter);
	    pTemp->setSource(pSource);
	    pSource = pTemp;
	}
	/* pSource is left pointing at the last source in the chain */

        /*
          Iterate through the resulting documents, and add them to the result.
        */
        BSONArrayBuilder resultArray; // where we'll stash the results
        for(bool hasDocument = !pSource->eof(); hasDocument;
                hasDocument = pSource->advance()) {
	    boost::intrusive_ptr<Document> pDocument(pSource->getCurrent());

            /* add the document to the result set */
            BSONObjBuilder documentBuilder;
            pDocument->toBson(&documentBuilder);
            resultArray.append(documentBuilder.done());
        }

        result.appendArray("result", resultArray.arr());

        return true;
    }

} // namespace mongo
